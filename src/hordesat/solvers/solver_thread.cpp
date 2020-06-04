
#include <sys/resource.h>

#include "solvers/solver_thread.h"
#include "HordeLib.h"
#include "utilities/hash.h"

using namespace SolvingStates;

void SolverThread::log(int verb, const char* fmt, ...) {
    va_list vl;
    va_start(vl, fmt);
    _logger->log_va_list(verb, fmt, vl);
    va_end(vl);
}

SolverThread::SolverThread(ParameterProcessor& params, std::shared_ptr<PortfolioSolverInterface> solver, 
        const std::vector<std::shared_ptr<std::vector<int>>>& formulae, const std::shared_ptr<vector<int>>& assumptions, 
        int localId, bool* finished) : 
    _params(params), _solver_ptr(solver), _solver(*solver), 
    _logger(params.getLogger().copy("S"+std::to_string(_solver.getGlobalId()))), 
    _formulae(formulae), _assumptions(assumptions), 
    _local_id(localId), _finished_flag(finished) {
    
    _portfolio_rank = _params.getIntParam("mpirank", 0);
    _portfolio_size = _params.getIntParam("mpisize", 1);

    _state = ACTIVE;
    _result = SatResult(UNKNOWN);
}

void SolverThread::start() {
    _thread = std::thread([&] {
        init();
        run();
    });
}

void SolverThread::init() {
    _tid = syscall(SYS_gettid);
    log(3, "tid %ld\n", _tid);
    if (_params.isSet("pin")) pin();
    _initialized = true;
}

void SolverThread::pin() {
    
    int solversCount = _params.getIntParam("c", 1);
	static int lastCpu = 0;
	int numCores = sysconf(_SC_NPROCESSORS_ONLN);
	int localRank = 0;
	const char* lranks = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
	if (lranks == NULL) {
		log(0, "WARNING: local rank was not determined\n");
	} else {
		localRank = atoi(lranks);
	}
	int desiredCpu = lastCpu + localRank*solversCount;
	lastCpu++;
	log(0, "Pinning thread to proc %d of %d, local rank is %d\n",
			desiredCpu, numCores, localRank);

	cpu_set_t cpuSet;
	CPU_ZERO(&cpuSet);
	CPU_SET(desiredCpu, &cpuSet);
	sched_setaffinity(0, sizeof(cpuSet), &cpuSet);
}

void* SolverThread::run() {
        
    while (!cancelThread()) {
        readFormula();
        if (cancelThread()) break;
        diversify();
    
        waitWhile(STANDBY);
        runOnce();
        waitWhile(STANDBY);
    }
    log(2, "exiting\n");
    return NULL;
}

void SolverThread::readFormula() {
    _initialized = false;
    log(3, "importing clauses\n");

    int prevLits = _imported_lits;
    int begin = _imported_lits;

    int i = 0;
    for (std::shared_ptr<std::vector<int>> f : _formulae) {
        if (begin < f->size())
            read(*f, begin);
        begin -= f->size();
        i++;
        //if (i < hlib->formulae.size() && cancelRun()) return;
        if (i < _formulae.size() && cancelThread()) return;
    }

    log(2, "imported cnf (%i lits)\n", _imported_lits-prevLits);
    log(1, "initialized\n");
}

void SolverThread::read(const std::vector<int>& formula, int begin) {
    int batchSize = 100000;
    for (int start = std::max(0, begin); start < (int) formula.size(); start += batchSize) {
        
        //waitWhile(SUSPENDED);
        //if (cancelRun()) break;
        if (cancelThread()) break;

        int limit = std::min(start+batchSize, (int) formula.size());
        for (int i = start; i < limit; i++) {
            _solver.addLiteral(formula[i]);
            _imported_lits++;
        }
    }
}

void SolverThread::diversify() {

	int diversificationMode = _params.getIntParam("d", 1);

    // Random seed: will be the same whenever rank and size stay the same,
    // changes to something completely new when rank or size change. 
    unsigned int seed = 42;
    hash_combine<unsigned int>(seed, (int)_tid);
    hash_combine<unsigned int>(seed, _portfolio_size);
    hash_combine<unsigned int>(seed, _portfolio_rank);  
    srand(seed);

	switch (diversificationMode) {
	case 1:
		log(3, "dv: sparse\n");
		sparseDiversification(_portfolio_size, _portfolio_rank);
		break;
	case 2:
		log(3, "dv: bin\n");
		binValueDiversification(_portfolio_size, _portfolio_rank);
		break;
	case 3:
		log(3, "dv: rand, s=%u\n", seed);
		randomDiversification();
		break;
	case 4:
		log(3, "dv: native\n");
		nativeDiversification(_portfolio_rank, _portfolio_size);
		break;
	case 5:
		log(3, "dv: sparse, native\n");
		sparseDiversification(_portfolio_size, _portfolio_rank);
		nativeDiversification(_portfolio_rank, _portfolio_size);
		break;
	case 6:
		log(3, "dv: sparserand, s=%u\n", seed);
		sparseRandomDiversification(_portfolio_size);
		break;
	case 7:
        if (_solver.getGlobalId() >= _solver.getNumOriginalDiversifications()) {
		    log(3, "dv: sparserand, native, s=%u\n", seed);
		    sparseRandomDiversification(_portfolio_size);
        } else {
            log(3, "dv: native, s=%u\n", seed);
        }
		nativeDiversification(_portfolio_rank, _portfolio_size);
		break;
	case 0:
		log(3, "dv: none\n");
		break;
	}
}

void SolverThread::sparseDiversification(int mpi_size, int mpi_rank) {

    int solversCount = _params.getIntParam("c", 1);
    int totalSolvers = mpi_size * solversCount;
    int vars = _solver.getVariablesCount();
    int shift = (mpi_rank * solversCount) + _local_id;

    for (int var = 1; var + totalSolvers < vars; var += totalSolvers) {
        _solver.setPhase(var + shift, true);
    }
}

void SolverThread::randomDiversification() {

    int vars = _solver.getVariablesCount();

    for (int var = 1; var <= vars; var++) {
        _solver.setPhase(var, rand()%2 == 1);
    }
}

void SolverThread::sparseRandomDiversification(int mpi_size) {

    int solversCount = _params.getIntParam("c", 1);
	int totalSolvers = solversCount * mpi_size;
    int vars = _solver.getVariablesCount();

    for (int var = 1; var <= vars; var++) {
        if (rand() % totalSolvers == 0) {
            _solver.setPhase(var, rand() % 2 == 1);
        }
    }
}

void SolverThread::nativeDiversification(int mpi_rank, int mpi_size) {

    int solversCount = _params.getIntParam("c", 1);
    _solver.diversify(_solver.getGlobalId(), mpi_size*solversCount);
}

void SolverThread::binValueDiversification(int mpi_size, int mpi_rank) {

    int solversCount = _params.getIntParam("c", 1);
	int totalSolvers = mpi_size * solversCount;
	int tmp = totalSolvers;
	int log = 0;
	while (tmp) {
		tmp >>= 1;
		log++;
	}
    int vars = _solver.getVariablesCount();
    int num = mpi_rank * _local_id;

    for (int var = 1; var < vars; var++) {
        int bit = var % log;
        bool phase = (num >> bit) & 1 ? true : false;
        _solver.setPhase(var, phase);
    }
}

void SolverThread::runOnce() {

    //hlib->hlog(3, "solverRunningThread, beginning main loop\n");
    while (true) {

        // Solving has just been done -> finish
        if (cancelRun()) break;

        // Wait as long as the thread is interrupted
        waitWhile(SUSPENDED);

        // Solving has been done now -> finish
        if (cancelRun()) break;

        //hlib->hlog(0, "rank %d starting solver with %d new lits, %d assumptions: %d\n", hlib->mpi_rank, litsAdded, hlib->assumptions.size(), hlib->assumptions[0]);
        log(3, "BEGSOL\n");
        SatResult res = _solver.solve(*_assumptions);
        log(3, "ENDSOL\n");

        // If interrupted externally
        if (cancelRun()) break;
        
        // Else, report result, if present
        if (res > 0) reportResult(res);
    }
}

void SolverThread::waitWhile(SolvingState state) {
    if (_state != state) return;
    log(3, "wait while %s\n", SolvingStateNames[state]);
    _state_cond.wait(_state_mutex, [&]{return _state != state;});
    log(3, "end wait\n");
}

bool SolverThread::cancelRun() {
    SolvingState s = _state;
    bool cancel = s == STANDBY || s == ABORTING;
    if (cancel) {
        log(1, "cancel run\n");
    }
    return cancel;
}

bool SolverThread::cancelThread() {
    SolvingState s = _state;
    bool cancel = s == ABORTING;
    return cancel;
}

void SolverThread::reportResult(int res) {
    log(3, "found result\n");
    if (res == SAT || res == UNSAT) {
        if (_state == ACTIVE) {
            log(-1, "found result %s\n", res==SAT?"SAT":"UNSAT");
            _result = SatResult(res);
            if (res == SAT) { 
                _solution = _solver.getSolution();
            } else {
                _failed_assumptions = _solver.getFailedAssumptions();
            }
            _state = STANDBY;
            *_finished_flag = true;
        }
    }
}

void SolverThread::setState(SolvingState state) {

    _state_mutex.lock();
    SolvingState oldState = _state;

    // (1) To STANDBY|ABORTING : Interrupt solver
    // (set signal to jump out of solving procedure)
    if (state == STANDBY || state == ABORTING) {
        _solver.interrupt();
        if (_tid >= 0) setpriority(PRIO_PROCESS, _tid, 15); // nice up thread
    }
    // (2) From STANDBY to !STANDBY : Restart solver
    else if (oldState == STANDBY && state != STANDBY) {
        _solver.uninterrupt();
        if (_tid >= 0) setpriority(PRIO_PROCESS, _tid, 0); // nice down thread
    }

    // (3) From !SUSPENDED to SUSPENDED : Suspend solvers 
    // (set signal to sleep inside solving procedure)
    if (oldState != SUSPENDED && state == SUSPENDED) {
        _solver.suspend();
    }
    // (4) From SUSPENDED to !SUSPENDED : Resume solvers
    // (set signal to wake up and resume solving procedure)
    if (oldState == SUSPENDED && state != SUSPENDED) {
        _solver.resume();
    }

    _state = state; 

    _state_mutex.unlock();
    _state_cond.notify();
}

SolverThread::~SolverThread() {}

const char* SolverThread::toStr() {
    _name = "S" + std::to_string(_solver.getGlobalId());
    return _name.c_str();
}
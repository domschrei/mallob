
#include "solvers/solver_thread.h"
#include "HordeLib.h"
#include "utilities/hash.h"

using namespace SolvingStates;

void log(int verb, const char* fmt, ...) {}

void pinThread(int solversCount) {
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

SolverThread::SolverThread(ParameterProcessor& params, std::shared_ptr<PortfolioSolverInterface> solver, 
        std::vector<std::shared_ptr<std::vector<int>>>& formulae, std::shared_ptr<vector<int>>& assumptions, 
        int localId) : 
    _params(params), _solver_ptr(solver), _solver(*solver), _formulae(formulae), _assumptions(assumptions), 
    _local_id(localId) {
    
    _portfolio_rank = _params.getIntParam("mpirank", 0);
    _portfolio_size = _params.getIntParam("mpisize", 1);

    std::string globalName = "<h-" + _params.getParam("jobstr") + "> " + std::string(toStr());
    _solver.setName(globalName);
    _state = ACTIVE;
    _result = SatResult(UNKNOWN);
}

void SolverThread::init() {
    _tid = syscall(SYS_gettid);
    log(3, "%s : tid %ld\n", toStr(), _tid);
    
    if (_params.isSet("pin")) {
        pinThread(_params.getIntParam("c", 1));
    }
}

void* SolverThread::run() {

    if (!cancelThread())
        waitWhile(INITIALIZING);
    if (!cancelThread())
        readFormula();
    if (!cancelThread())
        diversify();
    
    _initialized = true;

    while (!cancelThread()) {
    
        waitWhile(STANDBY);
        runOnce();
        waitWhile(STANDBY);
        
        if (cancelThread()) break;
        readFormula();
        diversify();
    }
    log(2, "%s exiting\n", toStr());
    return NULL;
}

void SolverThread::readFormula() {
    _initialized = false;
    log(3, "%s importing clauses\n", toStr());

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

    log(2, "%s imported cnf (%i lits)\n", toStr(), (_imported_lits-prevLits));
    log(1, "%s initialized\n", toStr());
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
		log(3, "dv: sparse, random, s=%u\n", seed);
		sparseRandomDiversification(_portfolio_size);
		break;
	case 7:
		log(3, "dv: sparse, random, native, s=%u\n", seed);
		sparseRandomDiversification(_portfolio_size);
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
    _solver.diversify(_solver.solverId, mpi_size*solversCount);
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
        log(3, "%s BEGSOL\n", toStr());
        SatResult res = _solver.solve(*_assumptions);
        log(3, "%s ENDSOL\n", toStr());

        // If interrupted externally
        if (cancelRun()) break;
        
        // Else, report result, if present
        if (res > 0) reportResult(res);
    }
}

void SolverThread::waitWhile(SolvingState state) {
    if (_state != state) return;
    _state_cond.wait(_state_mutex, [&]{return _state != state;});
}

bool SolverThread::cancelRun() {
    SolvingState s = _state;
    bool cancel = s == STANDBY || s == ABORTING;
    if (cancel) {
        log(1, "%s cancelling run\n", toStr());
    }
    return cancel;
}

bool SolverThread::cancelThread() {
    SolvingState s = _state;
    bool cancel = s == ABORTING;
    return cancel;
}

void SolverThread::reportResult(int res) {
    log(3,"%s found result\n", toStr());
    if (res == SAT || res == UNSAT) {
        if (_state == ACTIVE) {
            log(0,"%s found result %s\n", toStr(), res==SAT?"SAT":"UNSAT");
            _result = SatResult(res);
            if (res == SAT) { 
                _solution = _solver.getSolution();
            } else {
                _failed_assumptions = _solver.getFailedAssumptions();
            }
            _state = STANDBY;
        }
    }
}

SolverThread::~SolverThread() {}

const char* SolverThread::toStr() {
    _name = "S" + std::to_string(_solver.solverId);
    return _name.c_str();
}
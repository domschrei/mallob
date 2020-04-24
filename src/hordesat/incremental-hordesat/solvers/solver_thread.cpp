
#include "solvers/solver_thread.h"
#include "HordeLib.h"
#include "utilities/hash.h"

using namespace SolvingStates;

void pinThread(HordeLib& hlib, int solversCount) {
	static int lastCpu = 0;
	int numCores = sysconf(_SC_NPROCESSORS_ONLN);
	int localRank = 0;
	const char* lranks = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
	if (lranks == NULL) {
		hlib.hlog(0, "WARNING: local rank was not determined\n");
	} else {
		localRank = atoi(lranks);
	}
	int desiredCpu = lastCpu + localRank*solversCount;
	lastCpu++;
	hlib.hlog(0, "Pinning thread to proc %d of %d, local rank is %d\n",
			desiredCpu, numCores, localRank);

	cpu_set_t cpuSet;
	CPU_ZERO(&cpuSet);
	CPU_SET(desiredCpu, &cpuSet);
	sched_setaffinity(0, sizeof(cpuSet), &cpuSet);
}

SolverThread::SolverThread(HordeLib& hlib, PortfolioSolverInterface& solver, int localId) : 
    _hlib(hlib), _solver(solver), _local_id(localId) {}

void* SolverThread::run() {

    init();
    if (!cancelThread())
        waitWhile(INITIALIZING);
    if (!cancelThread())
        readFormula();
    if (!cancelThread())
        diversify();
    
    _hlib.solverThreadsInitialized[_local_id] = true;

    while (!cancelThread()) {
    
        waitWhile(STANDBY);
        runOnce();
        waitWhile(STANDBY);
        
        if (cancelThread()) break;
        readFormula();
        diversify();
    }
    _hlib.hlog(2, "%s exiting\n", toStr());
    return NULL;
}

void SolverThread::init() {

    if (_hlib.params.isSet("pin")) {
        pinThread(_hlib, _hlib.params.getIntParam("c", 1));
    }
    std::string globalName = "<h-" + _hlib.params.getParam("jobstr") + "> " + std::string(toStr());
    _solver.setName(globalName);
    
    long tid = syscall(SYS_gettid);
    _hlib.solverTids[_local_id] = tid;
    _hlib.hlog(3, "%s : tid %ld\n", toStr(), tid);
    
    //hlib->hlog(1, "solverRunningThread, entering\n");
    //hlib->solvingStateLock.lock();
    //hlib->solvingStateLock.unlock();
    _imported_lits = 0;
    _diversification_seed = std::tuple<int, int, int>(_hlib.mpi_rank, _hlib.mpi_size, 0);
}

void SolverThread::readFormula() {
    _hlib.solverThreadsInitialized[_local_id] = false;
    _hlib.hlog(3, "%s importing clauses\n", toStr());

    int prevLits = _imported_lits;
    int begin = _imported_lits;

    int i = 0;
    for (std::shared_ptr<std::vector<int>> f : _hlib.formulae) {
        if (begin < f->size())
            read(*f, begin);
        begin -= f->size();
        i++;
        //if (i < hlib->formulae.size() && cancelRun()) return;
        if (i < _hlib.formulae.size() && cancelThread()) return;
    }

    _hlib.hlog(2, "%s imported cnf (%i lits)\n", toStr(), (_imported_lits-prevLits));
    _hlib.hlog(1, "%s initialized\n", toStr());
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

	int diversificationMode = _hlib.params.getIntParam("d", 1);
    int mpi_size = _hlib.mpi_size;
    int mpi_rank = _hlib.mpi_rank;

    if (mpi_rank != std::get<0>(_diversification_seed) ||
        mpi_size != std::get<1>(_diversification_seed)) {
        
        // Rank or size changed: Change diversification
        _diversification_seed = std::tuple<int, int, int>(mpi_rank, mpi_size, (int)(100*_hlib.logger->getTime()));
    }

    // Random seed: will be the same whenever rank and size stay the same,
    // changes to something completely new when rank or size change. 
    int rank = std::get<0>(_diversification_seed);
    int size = std::get<1>(_diversification_seed);
    int time = std::get<2>(_diversification_seed);
    int sno = _local_id; // between 0 and <num-threads>-1
    unsigned int seed = 42;
    hash_combine<unsigned int>(seed, time);
    hash_combine<unsigned int>(seed, sno);        
    hash_combine<unsigned int>(seed, size);
    hash_combine<unsigned int>(seed, rank);  
    srand(seed);

	switch (diversificationMode) {
	case 1:
		_hlib.hlog(3, "dv: sparse\n");
		sparseDiversification(mpi_size, mpi_rank);
		break;
	case 2:
		_hlib.hlog(3, "dv: bin\n");
		binValueDiversification(mpi_size, mpi_rank);
		break;
	case 3:
		_hlib.hlog(3, "dv: rand, s=%u\n", seed);
		randomDiversification();
		break;
	case 4:
		_hlib.hlog(3, "dv: native\n");
		nativeDiversification(mpi_rank, mpi_size);
		break;
	case 5:
		_hlib.hlog(3, "dv: sparse, native\n");
		sparseDiversification(mpi_size, mpi_rank);
		nativeDiversification(mpi_rank, mpi_size);
		break;
	case 6:
		_hlib.hlog(3, "dv: sparse, random, s=%u\n", seed);
		sparseRandomDiversification(mpi_size);
		break;
	case 7:
		_hlib.hlog(3, "dv: sparse, random, native, s=%u\n", seed);
		sparseRandomDiversification(mpi_size);
		nativeDiversification(mpi_rank, mpi_size);
		break;
	case 0:
		_hlib.hlog(3, "dv: none\n");
		break;
	}
}

void SolverThread::sparseDiversification(int mpi_size, int mpi_rank) {

    int totalSolvers = mpi_size * _hlib.solversCount;
    int vars = _solver.getVariablesCount();
    int shift = (mpi_rank * _hlib.solversCount) + _local_id;

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

	int totalSolvers = _hlib.solversCount * mpi_size;
    int vars = _solver.getVariablesCount();

    for (int var = 1; var <= vars; var++) {
        if (rand() % totalSolvers == 0) {
            _solver.setPhase(var, rand() % 2 == 1);
        }
    }
}

void SolverThread::nativeDiversification(int mpi_rank, int mpi_size) {
    _solver.diversify(_solver.solverId, mpi_size*_hlib.solversCount);
}

void SolverThread::binValueDiversification(int mpi_size, int mpi_rank) {

	int totalSolvers = mpi_size * _hlib.solversCount;
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
        _hlib.hlog(3, "%s BEGSOL\n", toStr());
        SatResult res = _solver.solve(*_hlib.assumptions);
        _hlib.hlog(3, "%s ENDSOL\n", toStr());

        // If interrupted externally
        if (cancelRun()) break;
        
        // Else, report result, if present
        if (res > 0) reportResult(res);
    }
}

void SolverThread::waitWhile(SolvingState state) {
    if (_hlib.solvingState != state) return;
    _hlib.stateChangeCond.wait(_hlib.solvingStateLock, [&]{return _hlib.solvingState != state;});
}

bool SolverThread::cancelRun() {
    SolvingState s = _hlib.solvingState;
    bool cancel = s == STANDBY || s == ABORTING;
    if (cancel) {
        _hlib.hlog(1, "%s cancelling run\n", toStr());
    }
    return cancel;
}

bool SolverThread::cancelThread() {
    SolvingState s = _hlib.solvingState;
    bool cancel = s == ABORTING;
    return cancel;
}

void SolverThread::reportResult(int res) {
    _hlib.hlog(3,"%s found result\n", toStr());
    if (res == SAT || res == UNSAT) {
        auto lock = _hlib.solvingStateLock.getLock();
        if (_hlib.solvingState == ACTIVE) {
            _hlib.hlog(0,"%s found result %s\n", toStr(), res==SAT?"SAT":"UNSAT");
            _hlib.finalResult = SatResult(res);
            if (res == SAT) _hlib.truthValues = _solver.getSolution();
            else _hlib.failedAssumptions = _solver.getFailedAssumptions();
            _hlib.setSolvingState(STANDBY);
        }
    }
}

SolverThread::~SolverThread() {}

const char* SolverThread::toStr() {
    _name = "S" + std::to_string(_solver.solverId);
    return _name.c_str();
}
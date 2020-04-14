
#include "solvers/solver_thread.h"
#include "HordeLib.h"
#include "utilities/hash.h"

using namespace SolvingStates;

void pinThread(HordeLib* hlib, int solversCount) {
	static int lastCpu = 0;
	int numCores = sysconf(_SC_NPROCESSORS_ONLN);
	int localRank = 0;
	const char* lranks = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
	if (lranks == NULL) {
		hlib->hlog(0, "WARNING: local rank was not determined\n");
	} else {
		localRank = atoi(lranks);
	}
	int desiredCpu = lastCpu + localRank*solversCount;
	lastCpu++;
	hlib->hlog(0, "Pinning thread to proc %d of %d, local rank is %d\n",
			desiredCpu, numCores, localRank);

	cpu_set_t cpuSet;
	CPU_ZERO(&cpuSet);
	CPU_SET(desiredCpu, &cpuSet);
	sched_setaffinity(0, sizeof(cpuSet), &cpuSet);
}

SolverThread::SolverThread(void* args) {
    _args = (thread_args*)args;
}

void* SolverThread::run() {

    init();
    if (!cancelThread())
        waitWhile(INITIALIZING);
    if (!cancelThread())
        readFormula();
    if (!cancelThread())
        diversify();
    
    hlib->solverThreadsInitialized[_args->solverId] = true;

    while (!cancelThread()) {
    
        waitWhile(STANDBY);
        runOnce();
        waitWhile(STANDBY);
        
        if (cancelThread()) break;
        readFormula();
        diversify();
    }
    hlib->hlog(2, "%s exiting\n", toStr());
    return NULL;
}

void SolverThread::init() {

    hlib = _args->hlib;

    int localId = _args->solverId;
    solver = hlib->solvers[localId];
    if (hlib->params.isSet("pin")) {
        pinThread(hlib, hlib->params.getIntParam("c", 1));
    }
    std::string globalName = "<h-" + hlib->params.getParam("jobstr") + "> " + std::string(toStr());
    solver->setName(globalName);
    
    long tid = syscall(SYS_gettid);
    hlib->solverTids[_args->solverId] = tid;
    hlib->hlog(3, "%s : tid %ld\n", toStr(), tid);
    
    //hlib->hlog(1, "solverRunningThread, entering\n");
    //hlib->solvingStateLock.lock();
    //hlib->solvingStateLock.unlock();
    importedLits = 0;
    _diversification_seed = std::tuple<int, int, int>(0, 0, 0);
}

void SolverThread::readFormula() {
    hlib->solverThreadsInitialized[_args->solverId] = false;
    hlib->hlog(3, "%s importing clauses\n", toStr());

    int prevLits = importedLits;
    int begin = importedLits;

    int i = 0;
    for (std::shared_ptr<std::vector<int>> f : hlib->formulae) {
        if (begin < f->size())
            read(*f, begin);
        begin -= f->size();
        i++;
        //if (i < hlib->formulae.size() && cancelRun()) return;
        if (i < hlib->formulae.size() && cancelThread()) return;
    }

    hlib->hlog(2, "%s imported cnf (%i lits)\n", toStr(), (importedLits-prevLits));
    hlib->hlog(1, "%s initialized\n", toStr());
}

void SolverThread::read(const std::vector<int>& formula, int begin) {
    int batchSize = 100000;
    for (int start = std::max(0, begin); start < (int) formula.size(); start += batchSize) {
        
        //waitWhile(SUSPENDED);
        //if (cancelRun()) break;
        if (cancelThread()) break;

        int limit = std::min(start+batchSize, (int) formula.size());
        for (int i = start; i < limit; i++) {
            solver->addLiteral(formula[i]);
            importedLits++;
        }
    }
}

void SolverThread::diversify() {

	int diversificationMode = hlib->params.getIntParam("d", 1);
    int mpi_size = hlib->mpi_size;
    int mpi_rank = hlib->mpi_rank;

    if (mpi_rank != std::get<0>(_diversification_seed) ||
        mpi_size != std::get<1>(_diversification_seed)) {
        
        // Rank or size changed: New diversification needed
        _diversification_seed = std::tuple<int, int, int>(mpi_rank, mpi_size, (int)(100*getTime()));
    }

    // Random seed: will be the same whenever rank and size stay the same,
    // changes to something completely new when rank or size change. 
    int rank = std::get<0>(_diversification_seed);
    int size = std::get<1>(_diversification_seed);
    int time = std::get<2>(_diversification_seed);
    int sno = _args->solverId; // between 0 and <num-threads>-1
    unsigned int seed = 42;
    hash_combine<unsigned int>(seed, time);
    hash_combine<unsigned int>(seed, sno);        
    hash_combine<unsigned int>(seed, size);
    hash_combine<unsigned int>(seed, rank);  
    srand(seed);

	switch (diversificationMode) {
	case 1:
		sparseDiversification(mpi_size, mpi_rank);
		hlib->hlog(3, "dv: sparse\n");
		break;
	case 2:
		binValueDiversification(mpi_size, mpi_rank);
		hlib->hlog(3, "dv: bin\n");
		break;
	case 3:
		randomDiversification();
		hlib->hlog(3, "dv: rand, s=%u\n", seed);
		break;
	case 4:
		nativeDiversification(mpi_rank, mpi_size);
		hlib->hlog(3, "dv: native\n");
		break;
	case 5:
		sparseDiversification(mpi_size, mpi_rank);
		nativeDiversification(mpi_rank, mpi_size);
		hlib->hlog(3, "dv: sparse, native\n");
		break;
	case 6:
		sparseRandomDiversification(mpi_size);
		hlib->hlog(3, "dv: sparse, random, s=%u\n", seed);
		break;
	case 7:
		sparseRandomDiversification(mpi_size);
		nativeDiversification(mpi_rank, mpi_size);
		hlib->hlog(3, "dv: sparse, random, native, s=%u\n", seed);
		break;
	case 0:
		hlib->hlog(3, "dv: none\n");
		break;
	}
}

void SolverThread::sparseDiversification(int mpi_size, int mpi_rank) {
	int totalSolvers = mpi_size * hlib->solversCount;
    int vars = solver->getVariablesCount();

    int sid = _args->solverId;
    int shift = (mpi_rank * hlib->solversCount) + sid;
    for (int var = 1; var + totalSolvers < vars; var += totalSolvers) {
        solver->setPhase(var + shift, true);
    }
}

void SolverThread::randomDiversification() {
    int vars = solver->getVariablesCount();
    for (int var = 1; var <= vars; var++) {
        solver->setPhase(var, rand()%2 == 1);
    }
}

void SolverThread::sparseRandomDiversification(int mpi_size) {
	int totalSolvers = hlib->solversCount * mpi_size;
    int vars = solver->getVariablesCount();
    for (int var = 1; var <= vars; var++) {
        if (rand() % totalSolvers == 0) {
            solver->setPhase(var, rand() % 2 == 1);
        }
    }
}

void SolverThread::nativeDiversification(int mpi_rank, int mpi_size) {
    solver->diversify(solver->solverId, mpi_size*hlib->solversCount);
}

void SolverThread::binValueDiversification(int mpi_size, int mpi_rank) {
	int totalSolvers = mpi_size * hlib->solversCount;
	int tmp = totalSolvers;
	int log = 0;
	while (tmp) {
		tmp >>= 1;
		log++;
	}
    int vars = solver->getVariablesCount();
    int num = mpi_rank * _args->solverId;
    for (int var = 1; var < vars; var++) {
        int bit = var % log;
        bool phase = (num >> bit) & 1 ? true : false;
        solver->setPhase(var, phase);
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
        SatResult res = solver->solve(*hlib->assumptions);
        
        // If interrupted externally
        if (cancelRun()) break;
        
        // Else, report result, if present
        if (res > 0) reportResult(res);
    }
}

void SolverThread::waitWhile(SolvingState state) {

    if (hlib->solvingState != state) return;
    hlib->stateChangeCond.wait(hlib->solvingStateLock, [&]{return this->hlib->solvingState != state;});
}

bool SolverThread::cancelRun() {
    SolvingState s = hlib->solvingState;
    bool cancel = s == STANDBY || s == ABORTING;
    if (cancel) {
        hlib->hlog(1, "%s cancelling run\n", toStr());
    }
    return cancel;
}

bool SolverThread::cancelThread() {
    SolvingState s = hlib->solvingState;
    bool cancel = s == ABORTING;
    return cancel;
}

void SolverThread::reportResult(int res) {
    if (res == SAT || res == UNSAT) {
        auto lock = hlib->solvingStateLock.getLock();
        if (hlib->solvingState == ACTIVE) {
            hlib->hlog(0,"%s found result %s\n", toStr(), res==SAT?"SAT":"UNSAT");
            hlib->finalResult = SatResult(res);
            if (res == SAT) hlib->truthValues = solver->getSolution();
            else hlib->failedAssumptions = solver->getFailedAssumptions();
            hlib->setSolvingState(STANDBY);
        }
    }
}

SolverThread::~SolverThread() {
    if (hlib != NULL && _args != NULL)
        hlib->solverTids[_args->solverId] = -1;
    if (_args != NULL) delete _args;
}

const char* SolverThread::toStr() {
    _name = "S" + (solver == NULL ? std::string("?") : std::to_string(solver->solverId));
    return _name.c_str();
}
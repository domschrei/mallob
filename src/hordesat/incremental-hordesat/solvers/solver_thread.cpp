
#include "solvers/solver_thread.h"
#include "HordeLib.h"

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

void* SolverThread::run() {

    init();
    if (!cancelThread())
        waitWhile(INITIALIZING);
    if (!cancelThread())
        readFormula();
    if (!cancelThread())
        diversify();

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
    //hlib->hlog(1, "solverRunningThread, entering\n");
    //hlib->solvingStateLock.lock();
    int localId = _args->solverId;
    solver = hlib->solvers[localId];
    if (hlib->params.isSet("pin")) {
        pinThread(hlib, hlib->params.getIntParam("c", 1));
    }
    std::string globalName = "<h-" + hlib->params.getParam("jobstr") + "> " + std::string(toStr());
    solver->setName(globalName);
    //hlib->solvingStateLock.unlock();
    importedLits = 0;
}

void SolverThread::readFormula() {
    hlib->solverThreadsInitialized[_args->solverId] = false;
    hlib->hlog(1, "%s importing clauses\n", toStr());

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

    hlib->hlog(1, "%s imported clauses (%i lits)\n", toStr(), (importedLits-prevLits));
    hlib->hlog(1, "%s initialized\n", toStr());
    hlib->solverThreadsInitialized[_args->solverId] = true;
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

	int diversification = hlib->params.getIntParam("d", 1);
    int mpi_size = hlib->mpi_size;
    int mpi_rank = hlib->mpi_rank;
	switch (diversification) {
	case 1:
		sparseDiversification(mpi_size, mpi_rank);
		hlib->hlog(1, "doing sparse diversification\n");
		break;
	case 2:
		binValueDiversification(mpi_size, mpi_rank);
		hlib->hlog(1, "doing binary value based diversification\n");
		break;
	case 3:
		randomDiversification(2015);
		hlib->hlog(1, "doing random diversification\n");
		break;
	case 4:
		nativeDiversification(mpi_rank, mpi_size);
		hlib->hlog(1, "doing native diversification (plingeling)\n");
		break;
	case 5:
		sparseDiversification(mpi_size, mpi_rank);
		nativeDiversification(mpi_rank, mpi_size);
		hlib->hlog(1, "doing sparse + native diversification\n");
		break;
	case 6:
		sparseRandomDiversification(mpi_rank, mpi_size);
		hlib->hlog(1, "doing sparse random diversification\n");
		break;
	case 7:
		sparseRandomDiversification(mpi_rank, mpi_size);
		nativeDiversification(mpi_rank, mpi_size);
		hlib->hlog(1, "doing random sparse + native diversification (plingeling)\n");
		break;
	case 0:
		hlib->hlog(1, "no diversification\n");
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

void SolverThread::randomDiversification(unsigned int seed) {
	srand(seed);
    int vars = solver->getVariablesCount();
    for (int var = 1; var <= vars; var++) {
        solver->setPhase(var, rand()%2 == 1);
    }
}

void SolverThread::sparseRandomDiversification(unsigned int seed, int mpi_size) {
	srand(seed);
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

    //hlib->hlog(1, "solverRunningThread, beginning main loop\n");
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
        hlib->hlog(0, "%s cancelling run\n", toStr());
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
    delete _args;
}

const char* SolverThread::toStr() {
    _name = "S" + std::to_string(solver->solverId);
    return _name.c_str();
}
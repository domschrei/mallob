
#include "solvers/solver_thread.h"
#include "HordeLib.h"

using namespace SolvingStates;

void pinThread(int solversCount) {
	static int lastCpu = 0;
	int numCores = sysconf(_SC_NPROCESSORS_ONLN);
	int localRank = 0;
	const char* lranks = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
	if (lranks == NULL) {
		log(0, "WARNING: local rank was not determined.\n");
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

    init();
    waitWhile(INITIALIZING);
    readFormula();

    while (!cancelThread()) {
    
        waitWhile(STANDBY);
        runOnce();
        waitWhile(STANDBY);
        
        if (cancelThread()) break;
        readFormula();
    }
    log(2, "%s exiting\n", toStr());
    hlib->solverThreadsRunning[_args->solverId] = false;
    return NULL;
}

void SolverThread::init() {

    //log(1, "solverRunningThread, entering\n");
    hlib = _args->hlib;
    hlib->solvingStateLock.lock();
    int localId = _args->solverId;
    solver = hlib->solvers[localId];
    if (hlib->params.isSet("pin")) {
        pinThread(hlib->params.getIntParam("c", 1));
    }
    hlib->solvingStateLock.unlock();
    importedLits = 0;
}

void SolverThread::readFormula() {
    hlib->solverThreadsInitialized[_args->solverId] = false;
    log(1, "%s importing clauses\n", toStr());

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

    log(1, "%s imported clauses (%i lits)\n", toStr(), (importedLits-prevLits));
    log(1, "%s initialized\n", toStr());
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

void SolverThread::runOnce() {

    //log(1, "solverRunningThread, beginning main loop\n");
    while (true) {

        // Solving has just been done -> finish
        if (cancelRun()) break;

        // Wait as long as the thread is interrupted
        waitWhile(SUSPENDED);

        // Solving has been done now -> finish
        if (cancelRun()) break;

        //log(0, "rank %d starting solver with %d new lits, %d assumptions: %d\n", hlib->mpi_rank, litsAdded, hlib->assumptions.size(), hlib->assumptions[0]);
        hlib->finalResult = UNKNOWN;
        SatResult res = solver->solve(*hlib->assumptions);
        
        // If interrupted externally
        if (cancelRun()) break;
        
        // Else, report result, if present
        if (res > 0) reportResult(res);
    }
}

void SolverThread::waitWhile(SolvingState state) {

    hlib->solvingStateLock.lock();
    while (hlib->solvingState == state) {
        pthread_cond_wait(hlib->stateChangeCond.get(), hlib->solvingStateLock.mutex());
    }
    hlib->solvingStateLock.unlock();
}

bool SolverThread::cancelRun() {
    SolvingState s = hlib->solvingState;
    bool cancel = s == STANDBY || s == ABORTING;
    if (cancel) {
        log(0, "%s cancelling run\n", toStr());
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
        hlib->solvingStateLock.lock();
        if (hlib->solvingState == ACTIVE) {
            log(0,"%s found result %s\n", toStr(), res==SAT?"SAT":"UNSAT");
            hlib->finalResult = SatResult(res);
            if (res == SAT) hlib->truthValues = solver->getSolution();
            else hlib->failedAssumptions = solver->getFailedAssumptions();
            hlib->setSolvingState(STANDBY);
        }
        hlib->solvingStateLock.unlock();
    }
}

SolverThread::~SolverThread() {
    delete _args;
}

const char* SolverThread::toStr() {
    return ("S" + std::to_string(solver->solverId)).c_str();
}
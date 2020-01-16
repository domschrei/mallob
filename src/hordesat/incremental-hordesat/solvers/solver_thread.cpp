
#include "solvers/solver_thread.h"

#include <functional>

using namespace SolvingStates;

/*
void pinThread(HordeLib* hlib, int solversCount) {
	static int lastCpu = 0;
	int numCores = sysconf(_SC_NPROCESSORS_ONLN);
	int localRank = 0;
	const char* lranks = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
	if (lranks == NULL) {
		hlib->hlog(0, "WARNING: local rank was not determined.\n");
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
}*/

void* SolverThread::run() {

    init();
    //waitWhile(INITIALIZING);
    readFormula();

    while (!cancelThread()) {
    
        //waitWhile(STANDBY);
        runOnce();
        //waitWhile(STANDBY);
        
        if (cancelThread()) break;
        readFormula();
    }

    pause();

    //log(1, "-THREAD\n");
    _info->running = false;
    return NULL;
}

void SolverThread::init() {

    //register_termination_handler(SIGCHLD, std::bind(terminateSolverThread, this));

    //log(1, "+THREAD\n");
    //hlib->solvingStateLock.lock();
    int localId = _info->id;
    solver = _info->solver;
    //if (hlib->params.isSet("pin")) {
    //    pinThread(hlib, hlib->params.getIntParam("c", 1));
    //}
    //hlib->solvingStateLock.unlock();

    importedLits = 0;
}

void SolverThread::readFormula() {
    //hlib->hlog(1, "%s importing clauses\n", toStr());

    int prevLits = importedLits;
    int begin = importedLits;

    int i = 0;
    for (std::shared_ptr<std::vector<int>> f : _info->formulae) {
        if (begin < f->size())
            read(*f, begin);
        begin -= f->size();
        i++;
        //if (i < hlib->formulae.size() && cancelRun()) return;
        if (i < _info->formulae.size() && cancelThread()) return;
    }

    //hlib->hlog(1, "%s imported clauses (%i lits)\n", toStr(), (importedLits-prevLits));
    //hlib->hlog(1, "%s initialized\n", toStr());
    //hlib->solverThreadsInitialized[_args->solverId] = true;
    _info->initializing = false;
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

    //hlib->hlog(1, "solverRunningThread, beginning main loop\n");
    while (true) {

        // Solving has just been done -> finish
        if (cancelRun()) break;

        // Wait as long as the thread is interrupted
        //waitWhile(SUSPENDED);

        // Solving has been done now -> finish
        if (cancelRun()) break;

        //hlib->hlog(0, "rank %d starting solver with %d new lits, %d assumptions: %d\n", hlib->mpi_rank, litsAdded, hlib->assumptions.size(), hlib->assumptions[0]);
        SatResult res = solver->solve(*_info->assumptions);
        
        // If interrupted externally
        if (cancelRun()) break;
        
        // Else, report result, if present
        if (res > 0) reportResult(res);
    }
}

/*
void SolverThread::waitWhile(SolvingState state) {

    if (hlib->solvingState != state) return;
    hlib->solvingStateLock.lock();
    while (hlib->solvingState == state) {
        pthread_cond_wait(hlib->stateChangeCond.get(), hlib->solvingStateLock.mutex());
    }
    hlib->solvingStateLock.unlock();
}
*/

bool SolverThread::cancelRun() {
    return _info->interruptionSignal;
}

bool SolverThread::cancelThread() {
    return _info->interruptionSignal;
}

void SolverThread::reportResult(int res) {
    if (res == SAT || res == UNSAT) {
        //hlib->hlog(0,"%s found result %s\n", toStr(), res==SAT?"SAT":"UNSAT");
        SolverResult& solution = _info->result;
        solution.finalResult = SatResult(res);
        if (res == SAT) solution.truthValues = solver->getSolution();
        else solution.failedAssumptions = solver->getFailedAssumptions();
        _info->finished = true;
        //hlib->setSolvingState(STANDBY);
    }
}

SolverThread::~SolverThread() {
}

const char* SolverThread::toStr() {
    _name = "S" + std::to_string(solver->solverId);
    return _name.c_str();
}
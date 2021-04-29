
#include <sys/resource.h>
#include <assert.h>

#include "app/sat/hordesat/solvers/solver_thread.hpp"
#include "app/sat/hordesat/horde.hpp"
#include "app/sat/hordesat/utilities/hash.hpp"
#include "util/sys/proc.hpp"

using namespace SolvingStates;

SolverThread::SolverThread(const Parameters& params,
         std::shared_ptr<PortfolioSolverInterface> solver, 
        size_t fSize, const int* fLits, size_t aSize, const int* aLits,
        int localId, std::atomic_bool* finished) : 
    _params(params), _solver_ptr(solver), _solver(*solver), 
    _logger(_solver.getLogger()), 
    _f_size(fSize), _f_lits(fLits), _a_size(aSize), _a_lits(aLits),
    _shuffler(fSize, fLits), _local_id(localId), _finished_flag(finished) {
    
    _portfolio_rank = _params.getIntParam("apprank", 0);
    _portfolio_size = _params.getIntParam("mpisize", 1);

    _state = ACTIVE;
    _result = SatResult(UNKNOWN);
}

void SolverThread::start() {
    _thread = std::thread([this]() {
        init();
        run();
    });
}

void SolverThread::init() {
    _tid = Proc::getTid();
    _logger.log(V5_DEBG, "tid %ld\n", _tid);
    if (_params.isNotNull("pin")) pin();
    _initialized = true;
}

void SolverThread::pin() {
    
    int solversCount = _params.getIntParam("threads", 1);
	static int lastCpu = 0;
	int numCores = sysconf(_SC_NPROCESSORS_ONLN);
	int localRank = 0;
	const char* lranks = getenv("OMPI_COMM_WORLD_LOCAL_RANK");
	if (lranks == NULL) {
		_logger.log(V2_INFO, "WARNING: local rank was not determined\n");
	} else {
		localRank = atoi(lranks);
	}
	int desiredCpu = lastCpu + localRank*solversCount;
	lastCpu++;
	_logger.log(V2_INFO, "Pinning thread to proc %d of %d, local rank is %d\n",
			desiredCpu, numCores, localRank);

	cpu_set_t cpuSet;
	CPU_ZERO(&cpuSet);
	CPU_SET(desiredCpu, &cpuSet);
	sched_setaffinity(0, sizeof(cpuSet), &cpuSet);
}

void* SolverThread::run() {

    diversifyInitially();

    while (!cancelThread()) {
        readFormula();
        if (cancelThread()) break;
        diversifyAfterReading();
        _shuffler = ClauseShuffler(0, nullptr);
    
        waitWhile(STANDBY);
        runOnce();
        waitWhile(STANDBY);
    }
    _logger.log(V4_VVER, "exiting\n");
    return NULL;
}

void SolverThread::readFormula() {
    _logger.log(V5_DEBG, "importing clauses (%ld lits)\n", _f_size);
    size_t prevLits = _imported_lits;
    read();
    _logger.log(V4_VVER, "imported cnf (%ld lits)\n", _imported_lits-prevLits);
}

void SolverThread::read() {
    
    if (_shuffle) {

        while (!cancelThread() && _shuffler.hasNextClause()) {
            for (int lit : _shuffler.nextClause()) {
                _solver.addLiteral(lit);
                _imported_lits++;
            }
        }

    } else {

        int batchSize = 100000;
        for (size_t start = _imported_lits; start < _f_size; start += batchSize) {
            
            //waitWhile(SUSPENDED);
            //if (cancelRun()) break;
            if (cancelThread()) return;

            size_t limit = std::min(start+batchSize, _f_size);
            for (size_t i = start; i < limit; i++) {
                _solver.addLiteral(_f_lits[i]);
                _imported_lits++;
            }
        }
    }
}

void SolverThread::diversifyInitially() {

    // Random seed: will be the same whenever rank and size stay the same,
    // changes to something completely new when rank or size change. 
    unsigned int seed = 42;
    hash_combine<unsigned int>(seed, (int)_tid);
    hash_combine<unsigned int>(seed, _portfolio_size);
    hash_combine<unsigned int>(seed, _portfolio_rank);
    srand(seed);
    _solver.diversify(seed);

    // Shuffle input
    // ... only if original diversifications are exhausted
    _shuffle = _solver.getDiversificationIndex() >= _solver.getNumOriginalDiversifications();
    float random = 0.001f * (rand() % 1000); // random number in [0,1)
    assert(random >= 0); assert(random <= 1);
    // ... only if random throw hits user-defined probability
    _shuffle = _shuffle && random < _params.getFloatParam("shufinp");
    if (_shuffle) {
        _logger.log(V4_VVER, "Shuffling input\n");
        _shuffler.doShuffle();
    }
}

void SolverThread::diversifyAfterReading() {
	if (_solver.getGlobalId() >= _solver.getNumOriginalDiversifications()) {
        int solversCount = _params.getIntParam("threads", 1);
        int totalSolvers = solversCount * _portfolio_size;
        int vars = _solver.getVariablesCount();

        for (int var = 1; var <= vars; var++) {
            if (rand() % totalSolvers == 0) {
                _solver.setPhase(var, rand() % 2 == 1);
            }
        }
    }
}

void SolverThread::runOnce() {

    //hlib->h_logger.log(V5_DEBG, "solverRunningThread, beginning main loop\n");
    while (true) {

        // Solving has just been done -> finish
        if (cancelRun()) break;

        // Wait as long as the thread is interrupted
        waitWhile(SUSPENDED);

        // Solving has been done now -> finish
        if (cancelRun()) break;

        //hlib->h_logger.log(V2_INFO, "rank %d starting solver with %d new lits, %d assumptions: %d\n", hlib->mpi_rank, litsAdded, hlib->assumptions.size(), hlib->assumptions[0]);
        _logger.log(V5_DEBG, "BEGSOL\n");
        SatResult res = _solver.solve(_a_size, _a_lits);
        _logger.log(V5_DEBG, "ENDSOL\n");

        // If interrupted externally
        if (cancelRun()) break;
        
        // Else, report result, if present
        if (res > 0) reportResult(res);
    }
}

void SolverThread::waitWhile(SolvingState state) {
    if (_state != state) return;
    _logger.log(V5_DEBG, "wait while %s\n", SolvingStateNames[state]);
    _state_cond.wait(_state_mutex, [&]{return _state != state;});
    _logger.log(V5_DEBG, "end wait\n");
}

bool SolverThread::cancelRun() {
    SolvingState s = _state;
    bool cancel = s == STANDBY || s == ABORTING;
    if (cancel) {
        _logger.log(V3_VERB, "cancel run\n");
    }
    return cancel;
}

bool SolverThread::cancelThread() {
    SolvingState s = _state;
    bool cancel = s == ABORTING;
    return cancel;
}

void SolverThread::reportResult(int res) {
    _logger.log(V5_DEBG, "found result\n");
    if (res == SAT || res == UNSAT) {
        if (_state == ACTIVE) {
            _logger.log(V4_VVER, "found result %s\n", res==SAT?"SAT":"UNSAT");
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

SolverThread::~SolverThread() {
    if (_thread.joinable()) _thread.join();
}

const char* SolverThread::toStr() {
    _name = "S" + std::to_string(_solver.getGlobalId());
    return _name.c_str();
}
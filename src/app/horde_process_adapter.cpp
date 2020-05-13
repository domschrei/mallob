
#include "horde_process_adapter.h"

#include "HordeLib.h"
#include "util/shared_memory.h"
#include "util/memusage.h"
#include "util/timer.h"
#include "util/fork.h"

HordeProcessAdapter::HordeProcessAdapter(const std::map<std::string, std::string>& params, std::shared_ptr<LoggingInterface> loggingInterface, 
            const std::vector<std::shared_ptr<std::vector<int>>>& formulae, const std::shared_ptr<std::vector<int>>& assumptions) :
                _params(params), _log(loggingInterface), _formulae(formulae), _assumptions(assumptions) {

    _mutex = new SharedMemMutex(SharedMemory::create(SharedMemMutex::getSharedMemorySize()));
    _cond = new SharedMemConditionVariable(SharedMemory::create(SharedMemConditionVariable::getSharedMemorySize()));
    
    _import_buffer = (int*) SharedMemory::create(atoi(params.at("cbbs").c_str()) * sizeof(int) * atoi(params.at("mpisize").c_str()));
    _export_buffer = (int*) SharedMemory::create(atoi(params.at("cbbs").c_str()) * sizeof(int));
    _solution = (int*) SharedMemory::create(sizeof(int));

    _child_pid = (pid_t*) SharedMemory::create(sizeof(long));
    *_child_pid = -1;
    _state = (SolvingStates::SolvingState*) SharedMemory::create(sizeof(SolvingStates::SolvingState));
    *_state = SolvingStates::INITIALIZING;
    _portfolio_rank = (int*) SharedMemory::create(sizeof(int));
    *_portfolio_rank = atoi(params.at("mpirank").c_str());
    _portfolio_size = (int*) SharedMemory::create(sizeof(int));
    *_portfolio_size = atoi(params.at("mpisize").c_str());

    _import_buffer_size = (int*) SharedMemory::create(sizeof(int));
    *_import_buffer_size = 0;
    _export_buffer_size = (int*) SharedMemory::create(sizeof(int));
    *_export_buffer_size = 0;
    _do_import = (bool*) SharedMemory::create(sizeof(bool));
    *_do_import = false;
    _do_export = (bool*) SharedMemory::create(sizeof(bool));
    *_do_export = false;
    _did_export = (bool*) SharedMemory::create(sizeof(bool));
    *_did_export = false;
    _is_initialized = (bool*) SharedMemory::create(sizeof(bool));
    *_is_initialized = false;
    _do_write_solution = (bool*) SharedMemory::create(sizeof(bool));
    *_do_write_solution = false;
    _did_write_solution = (bool*) SharedMemory::create(sizeof(bool));
    *_did_write_solution = false;
    _do_dump_stats = (bool*) SharedMemory::create(sizeof(bool));
    *_do_dump_stats = false;
    _do_interrupt = (bool*) SharedMemory::create(sizeof(bool));
    *_do_interrupt = false;
    _do_update_role = (bool*) SharedMemory::create(sizeof(bool));
    *_do_update_role = false;
    _result = (SatResult*) SharedMemory::create(sizeof(SatResult));
    *_result = UNKNOWN;
}

void HordeProcessAdapter::run() {

    pid_t res = Fork::createChild();
    if (res > 0) {
        // [parent process] 
        // Success: write child PID, return to caller 
        *_child_pid = res;
        return;
    }

    // [child process]

    float startTime = Timer::elapsedSeconds();

    // Prepare solver
    HordeLib hlib(_params, _log);
    hlib.beginSolving(_formulae, _assumptions);
    *_state = SolvingStates::ACTIVE;

    // Main loop
    while (true) {

        //_log->log(0, "main loop\n");

        // Wait until something happens
        bool somethingHappened = _cond->timedWait(*_mutex, [&]() {
            // Done solving OR should {im|ex}port clauses OR should allocate shmem for solution
            return hlib.isAnySolutionFound() || *_do_export || *_do_import || *_do_update_role || *_do_write_solution;
        }, 1000 * 1000 /*1 millisecond*/);

        // Check initialization state
        if (!*_is_initialized && hlib.isFullyInitialized()) {
            *_is_initialized = true;
        }

        if (*_do_dump_stats) {
            // Dump stats
            float age = Timer::elapsedSeconds() - startTime;
            std::vector<long> threadTids = hlib.getSolverTids();
            for (int i = 0; i < threadTids.size(); i++) {
                if (threadTids[i] < 0) continue;
                double cpuRatio;
                thread_cpuratio(threadTids[i], age, cpuRatio);
                _log->log(0, "td.%i : %.2f%% CPU", threadTids[i], cpuRatio);
            }
            *_do_dump_stats = false;
        }

        if (*_do_interrupt) {
            hlib.interrupt();
            *_do_interrupt = false;
        }

        if (*_do_update_role) {
            _mutex->lock();
            hlib.updateRole(*_portfolio_rank, *_portfolio_size);
            *_do_update_role = false;
            _mutex->unlock();
        }

        if (!somethingHappened) continue;

        // Check if solution should be written into shared memory
        if (*_do_write_solution) {
            _mutex->lock();
            _solution = (int*)*_solution;
            memcpy(_solution, _solution_vec.data(), _solution_vec.size()*sizeof(int));
            *_do_write_solution = false;
            *_did_write_solution = true;
            _mutex->unlock();
            continue;
        }

        // Check if clauses should be exported
        if (*_do_export) {
            // Collect local clauses, put into shared memory
            // TODO do without copying all the data
            std::vector<int> clauses = hlib.prepareSharing(*_export_buffer_size);
            _mutex->lock();
            memcpy(_export_buffer, clauses.data(), clauses.size()*sizeof(int));
            *_export_buffer_size = clauses.size();
            *_do_export = false;
            *_did_export = true;
            _mutex->unlock();
            continue;
        }

        // Check if clauses should be imported
        if (*_do_import) {
            // Write imported clauses from shared memory into vector
            // TODO do without copying all the data
            _mutex->lock();
            std::vector<int> clauses(*_import_buffer_size);
            memcpy(clauses.data(), _import_buffer, *_import_buffer_size*sizeof(int));
            *_do_import = false;
            _mutex->unlock();
            hlib.digestSharing(clauses);
            continue;
        }

        // Check solved state
        if (*_result != UNKNOWN) continue;
        int result = hlib.solveLoop();
        if (result >= 0) {
            // Solution found!
            _mutex->lock();
            if (result == 10) {
                // SAT
                *_result = SAT;
                _solution_vec = hlib.getTruthValues();
            } else if (result == 20) {
                // UNSAT
                *_result = UNSAT; 
                std::set<int> fa = hlib.getFailedAssumptions();
                for (int x : fa) _solution_vec.push_back(x);
            }
            // Write size of solution such that main thread can allocate shared mem for it
            *_solution_size = _solution_vec.size();
            _mutex->unlock();
            continue;
        }
    }
}

bool HordeProcessAdapter::isFullyInitialized() {
    return *_is_initialized;
}

void HordeProcessAdapter::setSolvingState(SolvingStates::SolvingState state) {
    if (state == *_state) return;

    if (state == SolvingStates::ABORTING) {
        Fork::terminate(*_child_pid); // Terminate child process.
        return;
    }
    if (state == SolvingStates::SUSPENDED) {
        Fork::suspend(*_child_pid); // Stop (suspend) process.
        return;
    }
    if (state == SolvingStates::ACTIVE) {
        Fork::resume(*_child_pid); // Continue (resume) process.
        return;
    }
    if (state == SolvingStates::STANDBY) {
        *_do_interrupt = true;
    }
}

void HordeProcessAdapter::updateRole(int rank, int size) {
    _mutex->lock();
    *_portfolio_rank = rank;
    *_portfolio_size = size;
    *_do_update_role = true;
    _mutex->unlock();
    _cond->notify();
}

void HordeProcessAdapter::collectClauses(int maxSize) {
    _mutex->lock();
    *_export_buffer_size = maxSize;
    *_do_export = true;
    *_did_export = false;
    _mutex->unlock();
    _cond->notify();
}
bool HordeProcessAdapter::hasCollectedClauses() {
    return *_did_export;
}
std::vector<int> HordeProcessAdapter::getCollectedClauses() {
    _mutex->lock();
    std::vector<int> clauses(*_export_buffer_size);
    memcpy(clauses.data(), _export_buffer, clauses.size()*sizeof(int));
    *_did_export = false;
    _mutex->unlock();
}

void HordeProcessAdapter::digestClauses(const std::vector<int>& clauses) {
    _mutex->lock();
    *_import_buffer_size = clauses.size();
    memcpy(_import_buffer, clauses.data(), clauses.size()*sizeof(int));
    *_do_import = true;
    _mutex->unlock();
    _cond->notify();
}

void HordeProcessAdapter::dumpStats() {
    *_do_dump_stats = true;
}

bool HordeProcessAdapter::hasSolution() {
    if (*_result == SAT || *_result == UNSAT) {
        if (*_did_write_solution) {
            return true;
        }
        // Create shared memory block for solution
        *_solution = (int) SharedMemory::create(*_solution_size);
        *_do_write_solution = true;
        _cond->notify();
        return false;
    }
    return false;
}

std::pair<SatResult, std::vector<int>> HordeProcessAdapter::getSolution() {
    _mutex->lock();
    std::vector<int> solution(*_solution_size);
    memcpy(solution.data(), _solution, solution.size()*sizeof(int));
    _mutex->unlock();
    return std::pair<SatResult, std::vector<int>>(*_result, solution);
}
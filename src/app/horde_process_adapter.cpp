
#include <assert.h>
#include <sys/types.h>
#include <sys/syscall.h>

#include "horde_process_adapter.h"

#include "HordeLib.h"
#include "util/shared_memory.h"
#include "util/memusage.h"
#include "util/timer.h"
#include "util/fork.h"

HordeProcessAdapter::HordeProcessAdapter(const std::map<std::string, std::string>& params, std::shared_ptr<LoggingInterface> loggingInterface, 
            const std::vector<std::shared_ptr<std::vector<int>>>& formulae, const std::shared_ptr<std::vector<int>>& assumptions) :
                _params(params), _log(loggingInterface), _formulae(formulae), _assumptions(assumptions) {

    _max_import_buffer_size = atoi(params.at("cbbs").c_str()) * sizeof(int) * atoi(params.at("mpisize").c_str());
    _max_export_buffer_size = atoi(params.at("cbbs").c_str()) * sizeof(int);
    // Find maximum size of a potential solution
    int maxVar = 0;
    int minVar = 0;
    for (const auto& f : _formulae) {
        for (const int& lit : *f) {
            minVar = std::min(minVar, lit);
            maxVar = std::max(maxVar, lit);
        }
    }
    _max_solution_size = sizeof(int) * (std::max(maxVar, -minVar)+2);
    
    /*
    _shmem_mutex        = (void*)                        SharedMemory::create(SharedMemMutex::getSharedMemorySize());
    _shmem_cond         = (void*)                        SharedMemory::create(SharedMemConditionVariable::getSharedMemorySize());
    _import_buffer      = (int*)                         SharedMemory::create(_max_import_buffer_size);
    _export_buffer      = (int*)                         SharedMemory::create(_max_export_buffer_size);
    _solution           = (int*)                         SharedMemory::create(_max_solution_size);
    _child_pid          = (pid_t*)                       SharedMemory::create(sizeof(pid_t));
    _state              = (SolvingStates::SolvingState*) SharedMemory::create(sizeof(SolvingStates::SolvingState));
    _portfolio_rank     = (int*)                         SharedMemory::create(sizeof(int));
    _portfolio_size     = (int*)                         SharedMemory::create(sizeof(int));
    _import_buffer_size = (int*)                         SharedMemory::create(sizeof(int));
    _export_buffer_size = (int*)                         SharedMemory::create(sizeof(int));
    _solution_size      = (int*)                         SharedMemory::create(sizeof(int));
    _do_import          = (bool*)                        SharedMemory::create(sizeof(bool));
    _do_export          = (bool*)                        SharedMemory::create(sizeof(bool));
    _did_export         = (bool*)                        SharedMemory::create(sizeof(bool));
    _is_initialized     = (bool*)                        SharedMemory::create(sizeof(bool));
    _did_write_solution = (bool*)                        SharedMemory::create(sizeof(bool));
    _do_dump_stats      = (bool*)                        SharedMemory::create(sizeof(bool));
    _do_interrupt       = (bool*)                        SharedMemory::create(sizeof(bool));
    _do_update_role     = (bool*)                        SharedMemory::create(sizeof(bool));
    _result             = (SatResult*)                   SharedMemory::create(sizeof(SatResult));
    */

    initSharedMemory();
}

void HordeProcessAdapter::initSharedMemory() {

    // Initialize all needed chunks of shared memory
    _fields.emplace_back((void**) &_shmem_mutex,        SharedMemMutex::getSharedMemorySize());
    _fields.emplace_back((void**) &_shmem_cond,         SharedMemConditionVariable::getSharedMemorySize());
    _fields.emplace_back((void**) &_import_buffer,      _max_import_buffer_size);
    _fields.emplace_back((void**) &_export_buffer,      _max_export_buffer_size);
    _fields.emplace_back((void**) &_solution,           _max_solution_size);
    _fields.emplace_back((void**) &_child_pid,          sizeof(pid_t));
    _fields.emplace_back((void**) &_state,              sizeof(SolvingStates::SolvingState));
    _fields.emplace_back((void**) &_portfolio_rank,     sizeof(int));
    _fields.emplace_back((void**) &_portfolio_size,     sizeof(int));
    _fields.emplace_back((void**) &_import_buffer_size, sizeof(int));
    _fields.emplace_back((void**) &_export_buffer_size, sizeof(int));
    _fields.emplace_back((void**) &_solution_size,      sizeof(int));
    _fields.emplace_back((void**) &_do_import,          sizeof(bool));
    _fields.emplace_back((void**) &_do_export,          sizeof(bool));
    _fields.emplace_back((void**) &_did_export,         sizeof(bool));
    _fields.emplace_back((void**) &_is_initialized,     sizeof(bool));
    _fields.emplace_back((void**) &_did_write_solution, sizeof(bool));
    _fields.emplace_back((void**) &_do_dump_stats,      sizeof(bool));
    _fields.emplace_back((void**) &_do_interrupt,       sizeof(bool));
    _fields.emplace_back((void**) &_do_update_role,     sizeof(bool));
    _fields.emplace_back((void**) &_result,             sizeof(SatResult));

    // Allocate one block of shared memory for all fields
    _shmem_size = 0;
    for (const auto& field : _fields) {
        _shmem_size += field.second;
    }
    _shmem = SharedMemory::create(_shmem_size);

    // Set all fields to their respective position in the shared memory block
    void* memptr = _shmem;
    for (auto& field : _fields) {
        // Set the field's pointer to the next position in shared memory
        *(field.first) = memptr;
        memptr = memptr + field.second;
    }

    // Initialize fields
    _mutex = new SharedMemMutex(_shmem_mutex);
    _cond = new SharedMemConditionVariable(_shmem_cond);
    *_child_pid = -1;
    *_state = SolvingStates::INITIALIZING;
    *_portfolio_rank = atoi(_params.at("mpirank").c_str());
    *_portfolio_size = atoi(_params.at("mpisize").c_str());
    *_import_buffer_size = 0;
    *_export_buffer_size = 0;
    *_solution_size = 0;
    *_do_import = false;
    *_do_export = false;
    *_did_export = false;
    *_is_initialized = false;
    *_did_write_solution = false;
    *_do_dump_stats = false;
    *_do_interrupt = false;
    *_do_update_role = false;
    *_result = UNKNOWN;
}

HordeProcessAdapter::~HordeProcessAdapter() {
    delete _mutex;
    delete _cond;

    SharedMemory::free(_shmem, _shmem_size);

    /*
    SharedMemory::free(_shmem_mutex,        SharedMemMutex::getSharedMemorySize());
    SharedMemory::free(_shmem_cond,         SharedMemConditionVariable::getSharedMemorySize());
    SharedMemory::free(_import_buffer,      _max_import_buffer_size);
    SharedMemory::free(_export_buffer,      _max_export_buffer_size);
    SharedMemory::free(_solution,           _max_solution_size);
    SharedMemory::free(_child_pid,          sizeof(pid_t));
    SharedMemory::free(_state,              sizeof(SolvingStates::SolvingState));
    SharedMemory::free(_portfolio_rank,     sizeof(int));
    SharedMemory::free(_portfolio_size,     sizeof(int));
    SharedMemory::free(_import_buffer_size, sizeof(int));
    SharedMemory::free(_export_buffer_size, sizeof(int));
	SharedMemory::free(_solution_size,      sizeof(int));
    SharedMemory::free(_do_import,          sizeof(bool));
    SharedMemory::free(_do_export,          sizeof(bool));
    SharedMemory::free(_did_export,         sizeof(bool));
    SharedMemory::free(_is_initialized,     sizeof(bool));
    SharedMemory::free(_did_write_solution, sizeof(bool));
    SharedMemory::free(_do_dump_stats,      sizeof(bool));
    SharedMemory::free(_do_interrupt,       sizeof(bool));
    SharedMemory::free(_do_update_role,     sizeof(bool));
    SharedMemory::free(_result,             sizeof(SatResult));
    */
}

void HordeProcessAdapter::run() {

    pid_t res = Fork::createChild();
    if (res > 0) {
        // [parent process] 

        // Write child PID 
        *_child_pid = res;
        
        return;
    }

    // [child process]

    float startTime = Timer::elapsedSeconds();
    bool solved = false;

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
            return hlib.isAnySolutionFound() || *_do_export || *_do_import || *_do_update_role;
        }, 1000 * 1000 /*1 millisecond*/);

        // Check initialization state
        if (!*_is_initialized && hlib.isFullyInitialized()) {
            _log->log(3, "DO set initialized\n");
            *_is_initialized = true;
        }

        if (*_do_dump_stats) {
            // Dump stats
            _log->log(3, "DO dump stats\n");
            float age = Timer::elapsedSeconds() - startTime;

            // For this management thread
            double perc_cpu;
            bool success = thread_cpuratio(syscall(__NR_gettid), age, perc_cpu);
            if (success) {
                _log->log(0, "child_main : %.2f%% CPU", perc_cpu);
            }

            // For each solver thread
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
            _log->log(3, "DO interrupt\n");
            hlib.interrupt();
            *_do_interrupt = false;
        }

        if (*_do_update_role) {
            _log->log(3, "DO update role\n");
            _mutex->lock();
            hlib.updateRole(*_portfolio_rank, *_portfolio_size);
            *_do_update_role = false;
            _mutex->unlock();
        }

        //if (!somethingHappened) continue;

        // Check if clauses should be exported
        if (*_do_export) {
            _log->log(3, "DO export clauses\n");
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
            _log->log(3, "DO import clauses\n");
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
        if (solved) continue;
        int result = hlib.solveLoop();
        if (result >= 0) {
            solved = true;
            _log->log(3, "DO write solution\n");
            // Solution found!
            _mutex->lock();
            if (result == SatResult::SAT) {
                // SAT
                *_result = SAT;
                _solution_vec = hlib.getTruthValues();
            } else if (result == SatResult::UNSAT) {
                // UNSAT
                *_result = UNSAT; 
                std::set<int> fa = hlib.getFailedAssumptions();
                for (int x : fa) _solution_vec.push_back(x);
            }
            // Write size of solution such that main thread can allocate shared mem for it
            *_solution_size = _solution_vec.size();
            if (*_solution_size > 0) {
                memcpy(_solution, _solution_vec.data(), *_solution_size*sizeof(int));
            }
            *_did_write_solution = true;
            _mutex->unlock();
            _log->log(3, "DONE write solution\n");
            continue;
        }
    }
}

bool HordeProcessAdapter::isFullyInitialized() {
    return *_is_initialized;
}

pid_t HordeProcessAdapter::getPid() {
    return *_child_pid;
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
    if (*_do_export) return; // already collecting
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
    return clauses;
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
    return *_did_write_solution;
}

std::pair<SatResult, std::vector<int>> HordeProcessAdapter::getSolution() {
    if (*_solution_size == 0) return std::pair<SatResult, std::vector<int>>(*_result, std::vector<int>()); 
    _mutex->lock();
    std::vector<int> solution(*_solution_size);
    memcpy(solution.data(), _solution, solution.size()*sizeof(int));
    _mutex->unlock();
    return std::pair<SatResult, std::vector<int>>(*_result, solution);
}
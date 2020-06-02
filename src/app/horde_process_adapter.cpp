
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
            const std::vector<std::shared_ptr<std::vector<int>>>& formulae, const std::shared_ptr<std::vector<int>>& assumptions, 
            int numVars) :
                _params(params), _log(loggingInterface), _formulae(formulae), _assumptions(assumptions) {

    _max_import_buffer_size = atoi(params.at("cbbs").c_str()) * sizeof(int) * atoi(params.at("mpisize").c_str());
    _max_export_buffer_size = atoi(params.at("cbbs").c_str()) * sizeof(int);
    _max_solution_size = sizeof(int) * (numVars+1);

    initSharedMemory();
}

void HordeProcessAdapter::initSharedMemory() {

    // Initialize all needed chunks of shared memory
    
    std::vector<std::pair<void**, int>> fields;
    
    // a) R&W by parent, readonly by child

    fields.emplace_back((void**) &_child_pid,               sizeof(pid_t));
    fields.emplace_back((void**) &_portfolio_rank,          sizeof(int));
    fields.emplace_back((void**) &_portfolio_size,          sizeof(int));

    fields.emplace_back((void**) &_do_export,               sizeof(bool));
    fields.emplace_back((void**) &_do_import,               sizeof(bool));
    fields.emplace_back((void**) &_do_dump_stats,           sizeof(bool));
    fields.emplace_back((void**) &_do_update_role,          sizeof(bool));
    fields.emplace_back((void**) &_do_interrupt,            sizeof(bool));

    fields.emplace_back((void**) &_export_buffer_max_size,  sizeof(int));
    fields.emplace_back((void**) &_import_buffer_size,      sizeof(int));
    fields.emplace_back((void**) &_import_buffer,           _max_import_buffer_size);

    // b) R&W by child, readonly by parent

    fields.emplace_back((void**) &_did_export,              sizeof(bool));
    fields.emplace_back((void**) &_did_import,              sizeof(bool));
    fields.emplace_back((void**) &_did_dump_stats,          sizeof(bool));
    fields.emplace_back((void**) &_did_update_role,         sizeof(bool));
    fields.emplace_back((void**) &_did_interrupt,           sizeof(bool));

    fields.emplace_back((void**) &_is_initialized,          sizeof(bool));
    fields.emplace_back((void**) &_has_solution,            sizeof(bool));
    fields.emplace_back((void**) &_result,                  sizeof(SatResult));
    fields.emplace_back((void**) &_solution_size,           sizeof(int));
    fields.emplace_back((void**) &_solution,                _max_solution_size);
    
    fields.emplace_back((void**) &_export_buffer_true_size, sizeof(int));
    fields.emplace_back((void**) &_export_buffer,           _max_export_buffer_size);
    
    // Allocate one block of shared memory for all fields
    _shmem_size = 0;
    for (const auto& field : fields) {
        _shmem_size += field.second;
    }
    _shmem = SharedMemory::create(_shmem_size);

    // Set all fields to their respective position in the shared memory block
    void* memptr = _shmem;
    for (auto& field : fields) {
        // Set the field's pointer to the next position in shared memory
        *(field.first) = memptr;
        // Advance memptr by <field.second> bytes
        memptr = ((char*)memptr) + field.second;
    }

    // Initialize fields
    
    *_child_pid = -1;
    *_portfolio_rank = atoi(_params.at("mpirank").c_str());
    *_portfolio_size = atoi(_params.at("mpisize").c_str());

    *_do_export = false;
    *_do_import = false;
    *_do_dump_stats = false;
    *_do_update_role = false;
    *_do_interrupt = false;

    *_export_buffer_max_size = 0;
    *_import_buffer_size = 0;

    *_did_export = false;
    *_did_import = false;
    *_did_dump_stats = false;
    *_did_update_role = false;
    *_did_interrupt = false;

    *_is_initialized = false;
    *_has_solution = false;
    *_result = UNKNOWN;
    *_solution_size = 0;

    *_export_buffer_true_size = 0;
}

HordeProcessAdapter::~HordeProcessAdapter() {
    
    if (*_child_pid != getpid()) {
        // Parent process
        SharedMemory::free(_shmem, _shmem_size);
    }

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
        _log->log(1, "Child pid=%i created\n", *_child_pid);
        _state = SolvingStates::ACTIVE;
        
        return;
    }

    // [child process]

    Fork::init(Fork::_rank);
    
    // Prepare solver
    HordeLib hlib(_params, _log);
    hlib.beginSolving(_formulae, _assumptions);
    bool interrupted = false;

    // Main loop
    while (true) {

        //_log->log(0, "main loop\n");

        // Wait until something happens
        // (can be interrupted by Fork::wakeUp(*_child_pid))
        float time = Timer::elapsedSeconds();
        int sleepStatus = usleep(1000 /*1 millisecond*/);
        time = Timer::elapsedSeconds() - time;
        if (sleepStatus != 0) _log->log(3, "Interrupted; slept for %i us\n", (int) (1000*1000*time));

        // Interrupt solvers
        if (*_do_interrupt && !*_did_interrupt) {
            _log->log(3, "DO interrupt\n");
            hlib.interrupt();
            *_did_interrupt = true;
            interrupted = true;
        }
        if (!*_do_interrupt) *_did_interrupt = false;

        // Dump stats
        if (!interrupted && *_do_dump_stats && !*_did_dump_stats) {
            _log->log(3, "DO dump stats\n");
            
            // For this management thread
            double perc_cpu; float sysShare;
            bool success = Proc::getThreadCpuRatio(Proc::getTid(), perc_cpu, sysShare);
            if (success) {
                _log->log(0, "child_main : %.2f%% CPU -> %.2f%% systime", perc_cpu, sysShare*100);
            }

            // For each solver thread
            std::vector<long> threadTids = hlib.getSolverTids();
            for (int i = 0; i < threadTids.size(); i++) {
                if (threadTids[i] < 0) continue;
                
                success = Proc::getThreadCpuRatio(threadTids[i], perc_cpu, sysShare);
                if (success) {
                    _log->log(0, "td.%ld : %.2f%% CPU -> %.2f%% systime", threadTids[i], perc_cpu, sysShare*100);
                }
            }

            *_did_dump_stats = true;
        }
        if (!*_do_dump_stats) *_did_dump_stats = false;

        // Update role
        if (*_do_update_role && !*_did_update_role) {
            _log->log(3, "DO update role\n");
            hlib.updateRole(*_portfolio_rank, *_portfolio_size);
            *_did_update_role = true;
        }
        if (!*_do_update_role) *_did_update_role = false;

        // Check if clauses should be exported
        if (!interrupted && *_do_export && !*_did_export) {
            _log->log(3, "DO export clauses\n");
            // Collect local clauses, put into shared memory
            *_export_buffer_true_size = hlib.prepareSharing(_export_buffer, *_export_buffer_max_size);
            *_did_export = true;
        }
        if (!*_do_export) *_did_export = false;

        // Check if clauses should be imported
        if (!interrupted && *_do_import && !*_did_import) {
            _log->log(3, "DO import clauses\n");
            // Write imported clauses from shared memory into vector
            hlib.digestSharing(_import_buffer, *_import_buffer_size);
            *_did_import = true;
        }
        if (!*_do_import) *_did_import = false;

        // Check initialization state
        if (!interrupted && !*_is_initialized && hlib.isFullyInitialized()) {
            _log->log(3, "DO set initialized\n");
            *_is_initialized = true;
        }

        // Check solved state
        if (interrupted || *_has_solution) continue;
        int result = hlib.solveLoop();
        if (result >= 0) {
            _log->log(3, "DO write solution\n");
            // Solution found!
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
            // Write solution
            *_solution_size = _solution_vec.size();
            if (*_solution_size > 0) {
                memcpy(_solution, _solution_vec.data(), *_solution_size*sizeof(int));
            }
            _log->log(3, "DONE write solution\n");
            *_has_solution = true;
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
    if (state == _state) return;

    if (state == SolvingStates::ABORTING) {
        Fork::terminate(*_child_pid); // Terminate child process.
    }
    if (state == SolvingStates::SUSPENDED) {
        Fork::suspend(*_child_pid); // Stop (suspend) process.
    }
    if (state == SolvingStates::ACTIVE) {
        Fork::resume(*_child_pid); // Continue (resume) process.
    }
    if (state == SolvingStates::STANDBY) {
        *_do_interrupt = true;
    }

    _state = state;
}

void HordeProcessAdapter::updateRole(int rank, int size) {
    *_portfolio_rank = rank;
    *_portfolio_size = size;
    *_do_update_role = true;
}

void HordeProcessAdapter::collectClauses(int maxSize) {
    *_export_buffer_max_size = maxSize;
    *_do_export = true;
    Fork::wakeUp(*_child_pid);
}
bool HordeProcessAdapter::hasCollectedClauses() {
    return *_did_export;
}
std::vector<int> HordeProcessAdapter::getCollectedClauses() {
    std::vector<int> clauses(*_export_buffer_true_size);
    memcpy(clauses.data(), _export_buffer, clauses.size()*sizeof(int));
    *_do_export = false;
    return clauses;
}

void HordeProcessAdapter::digestClauses(const std::vector<int>& clauses) {
    *_import_buffer_size = clauses.size();
    memcpy(_import_buffer, clauses.data(), clauses.size()*sizeof(int));
    *_do_import = true;
    Fork::wakeUp(*_child_pid);
}

void HordeProcessAdapter::dumpStats() {
    *_do_dump_stats = true;
    // No hard need to wake up immediately
}

bool HordeProcessAdapter::check() {
    if (*_did_import) *_do_import = false;
    if (*_did_update_role) *_do_update_role = false;
    if (*_did_interrupt) *_do_interrupt = false;
    if (*_did_dump_stats) *_do_dump_stats = false;
    return *_has_solution;
}

std::pair<SatResult, std::vector<int>> HordeProcessAdapter::getSolution() {
    if (*_solution_size == 0) return std::pair<SatResult, std::vector<int>>(*_result, std::vector<int>()); 
    std::vector<int> solution(*_solution_size);
    memcpy(solution.data(), _solution, solution.size()*sizeof(int));
    return std::pair<SatResult, std::vector<int>>(*_result, solution);
}
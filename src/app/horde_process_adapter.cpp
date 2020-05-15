
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
    float startTime = Timer::elapsedSeconds();

    // Prepare solver
    HordeLib hlib(_params, _log);
    hlib.beginSolving(_formulae, _assumptions);

    // Main loop
    while (true) {

        //_log->log(0, "main loop\n");

        // Wait until something happens
        usleep(1000 /*1 millisecond*/);

        // Check initialization state
        if (!*_is_initialized && hlib.isFullyInitialized()) {
            _log->log(3, "DO set initialized\n");
            *_is_initialized = true;
        }

        // Dump stats
        if (*_do_dump_stats && !*_did_dump_stats) {
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

            *_did_dump_stats = true;
        }
        if (!*_do_dump_stats) *_did_dump_stats = false;

        // Interrupt solvers
        if (*_do_interrupt && !*_did_interrupt) {
            _log->log(3, "DO interrupt\n");
            hlib.interrupt();
            *_did_interrupt = true;
        }
        if (!*_do_interrupt) *_did_interrupt = false;

        // Update role
        if (*_do_update_role && !*_did_update_role) {
            _log->log(3, "DO update role\n");
            hlib.updateRole(*_portfolio_rank, *_portfolio_size);
            *_did_update_role = true;
        }
        if (!*_do_update_role) *_did_update_role = false;

        // Check if clauses should be exported
        if (*_do_export && !*_did_export) {
            _log->log(3, "DO export clauses\n");
            // Collect local clauses, put into shared memory
            // TODO do without copying all the data
            std::vector<int> clauses = hlib.prepareSharing(*_export_buffer_max_size);
            memcpy(_export_buffer, clauses.data(), clauses.size()*sizeof(int));
            *_export_buffer_true_size = clauses.size();
            *_did_export = true;
        }
        if (!*_do_export) *_did_export = false;

        // Check if clauses should be imported
        if (*_do_import && !*_did_import) {
            _log->log(3, "DO import clauses\n");
            // Write imported clauses from shared memory into vector
            // TODO do without copying all the data
            std::vector<int> clauses(*_import_buffer_size);
            memcpy(clauses.data(), _import_buffer, *_import_buffer_size*sizeof(int));
            *_did_import = true;
            hlib.digestSharing(clauses);
        }
        if (!*_do_import) *_did_import = false;

        // Check solved state
        if (*_has_solution) continue;
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
}

void HordeProcessAdapter::dumpStats() {
    *_do_dump_stats = true;
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
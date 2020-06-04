
#ifndef DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H
#define DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H

#include <map>

#include "hordesat/utilities/logging_interface.hpp"
#include "util/sys/threading.hpp"
#include "hordesat/solvers/solving_state.hpp"
#include "hordesat/solvers/portfolio_solver_interface.hpp"

class HordeProcessAdapter {

private:
    const std::map<std::string, std::string>& _params;
    std::shared_ptr<LoggingInterface> _log;

    const std::vector<std::shared_ptr<std::vector<int>>>& _formulae; 
    const std::shared_ptr<std::vector<int>>& _assumptions;

    std::vector<int> _solution_vec;

    SolvingStates::SolvingState _state;

    int _max_import_buffer_size;
    int _max_export_buffer_size;
    int _max_solution_size;
    
    size_t _shmem_size;

    // SHARED MEMORY
    
    // Pointer to entire block of shared memory
    void* _shmem;

    // Meta data parent->child
    pid_t* _child_pid;
    int* _portfolio_rank;
    int* _portfolio_size;

    // Instructions parent->child
    bool* _do_export;
    bool* _do_import;
    bool* _do_dump_stats;
    bool* _do_update_role;
    bool* _do_interrupt;

    // Responses child->parent
    bool* _did_export;
    bool* _did_import;
    bool* _did_dump_stats;
    bool* _did_update_role;
    bool* _did_interrupt;

    // State alerts child->parent
    bool* _is_spawned;
    bool* _is_initialized;
    bool* _has_solution;
    SatResult* _result = NULL;
	int* _solution_size;
    int* _solution;
    
    // Clause buffers: parent->child
    int* _export_buffer_max_size;
    int* _import_buffer_size;
    int* _import_buffer;
    
    // Clause buffers: child->parent
    int* _export_buffer_true_size;
    int* _export_buffer;



public:
    HordeProcessAdapter(const std::map<std::string, std::string>& params, std::shared_ptr<LoggingInterface> loggingInterface, 
            const std::vector<std::shared_ptr<std::vector<int>>>& formulae, const std::shared_ptr<std::vector<int>>& assumptions,
            int numVars);
    ~HordeProcessAdapter();

    /*
    Returns the PID of the spawned child process.
    */
    pid_t run();
    bool isFullyInitialized();
    pid_t getPid();

    void setSolvingState(SolvingStates::SolvingState state);
    void updateRole(int rank, int size);

    void collectClauses(int maxSize);
    bool hasCollectedClauses();
    std::vector<int> getCollectedClauses();
    void digestClauses(const std::vector<int>& clauses);

    void dumpStats();
    
    bool check();
    std::pair<SatResult, std::vector<int>> getSolution();

private:
    void initSharedMemory();

};

#endif
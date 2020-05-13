
#ifndef DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H
#define DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H

#include <map>

#include "utilities/logging_interface.h"
#include "utilities/Threading.h"
#include "solvers/solving_state.h"
#include "solvers/PortfolioSolverInterface.h"

class HordeProcessAdapter {

private:
    const std::map<std::string, std::string>& _params;
    std::shared_ptr<LoggingInterface> _log;

    const std::vector<std::shared_ptr<std::vector<int>>>& _formulae; 
    const std::shared_ptr<std::vector<int>>& _assumptions;

    std::vector<int> _solution_vec;

    // SHARED MEMORY

    pid_t* _child_pid;
    SharedMemMutex* _mutex;
    SharedMemConditionVariable* _cond;
    SolvingStates::SolvingState* _state;

    int* _portfolio_rank;
    int* _portfolio_size;

    // Instructions main->solver
    bool* _do_export;
    bool* _do_import;
    bool* _do_dump_stats;
    bool* _do_update_role;
    bool* _do_interrupt;
    bool* _do_write_solution;

    // Responses solver->main
    bool* _is_initialized;
    bool* _did_export;
    bool* _did_write_solution;
    
    // Clause buffers to be exchanged
    int* _export_buffer_size;
    int* _export_buffer;
    int* _import_buffer_size;
    int* _import_buffer;

    // Solution
    SatResult* _result = NULL;
	int* _solution_size;
    void** _solution;


public:
    HordeProcessAdapter(const std::map<std::string, std::string>& params, std::shared_ptr<LoggingInterface> loggingInterface, 
            const std::vector<std::shared_ptr<std::vector<int>>>& formulae, const std::shared_ptr<std::vector<int>>& assumptions);

    void run();
    bool isFullyInitialized();

    void setSolvingState(SolvingStates::SolvingState state);
    void updateRole(int rank, int size);

    void collectClauses(int maxSize);
    bool hasCollectedClauses();
    std::vector<int> getCollectedClauses();
    void digestClauses(const std::vector<int>& clauses);

    void dumpStats();
    
    bool hasSolution();
    std::pair<SatResult, std::vector<int>> getSolution();

};

#endif
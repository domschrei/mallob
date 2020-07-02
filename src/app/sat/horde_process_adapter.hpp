
#ifndef DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H
#define DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H

#include <map>

#include "hordesat/utilities/logging_interface.hpp"
#include "util/sys/threading.hpp"
#include "util/params.hpp"
#include "util/sys/pipe.hpp"
#include "hordesat/solvers/solving_state.hpp"
#include "hordesat/solvers/portfolio_solver_interface.hpp"
#include "horde_shared_memory.hpp"

class HordeProcessAdapter {

private:
    Parameters _params;
    std::shared_ptr<LoggingInterface> _log;

    const std::vector<std::shared_ptr<std::vector<int>>>& _formulae; 
    const std::shared_ptr<std::vector<int>>& _assumptions;

    std::vector<std::tuple<std::string, void*, int>> _shmem;
    std::string _shmem_id;
    HordeSharedMemory* _hsm;

    int* _export_buffer;
    int* _import_buffer;

    pid_t _child_pid;
    SolvingStates::SolvingState _state;

public:
    HordeProcessAdapter(const Parameters& params,
            const std::vector<std::shared_ptr<std::vector<int>>>& formulae, const std::shared_ptr<std::vector<int>>& assumptions);
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
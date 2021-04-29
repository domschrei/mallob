
#ifndef DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H
#define DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H

#include <list>

#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "util/params.hpp"
#include "hordesat/solvers/solving_state.hpp"
#include "hordesat/solvers/portfolio_solver_interface.hpp"
#include "horde_shared_memory.hpp"

class HordeProcessAdapter {

public:
    struct RevisionData {
        int revision;
        size_t fSize;
        const int* fLits;
        size_t aSize;
        const int* aLits;
    };

private:
    Parameters _params;
    std::shared_ptr<Logger> _log;

    size_t _f_size;
    const int* _f_lits;
    size_t _a_size;
    const int* _a_lits;
    int _revision_update;
    
    std::vector<std::tuple<std::string, void*, int>> _shmem;
    std::string _shmem_id;
    HordeSharedMemory* _hsm;

    int* _export_buffer;
    int* _import_buffer;

    pid_t _child_pid;
    SolvingStates::SolvingState _state = SolvingStates::INITIALIZING;

public:
    HordeProcessAdapter(const Parameters& params, 
            size_t fSize, const int* fLits, size_t aSize, const int* aLits);
    ~HordeProcessAdapter();

    /*
    Returns the PID of the spawned child process.
    */
    pid_t run();
    bool isFullyInitialized();
    pid_t getPid();

    void appendRevisions(const std::vector<RevisionData>& revisions);

    void setSolvingState(SolvingStates::SolvingState state);

    void collectClauses(int maxSize);
    bool hasCollectedClauses();
    std::vector<int> getCollectedClauses();
    void digestClauses(const std::vector<int>& clauses);

    void dumpStats();
    
    bool check();
    std::pair<SatResult, std::vector<int>> getSolution();

    void freeSharedMemory();

private:
    void initSharedMemory();
    void* createSharedMemoryBlock(std::string shmemSubId, size_t size, void* data);

};

#endif

#ifndef DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H
#define DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H

#include <list>

#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "util/params.hpp"
#include "hordesat/solvers/solving_state.hpp"
#include "hordesat/solvers/portfolio_solver_interface.hpp"
#include "horde_shared_memory.hpp"
#include "data/checksum.hpp"

class HordeProcessAdapter {

public:
    struct RevisionData {
        int revision;
        Checksum checksum;
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
    int _revision_update = -1;
    
    struct ShmemObject {
        std::string id; 
        void* data; 
        size_t size;
        bool operator==(const ShmemObject& other) const {
            return id == other.id && size == other.size;
        }
    };
    struct ShmemObjectHasher {
        size_t operator()(const ShmemObject& obj) const {
            size_t hash = 1;
            hash_combine(hash, obj.id);
            hash_combine(hash, obj.size);
            return hash;
        }
    };
    robin_hood::unordered_flat_set<ShmemObject, ShmemObjectHasher> _shmem;
    std::string _shmem_id;
    HordeSharedMemory* _hsm = nullptr;

    int* _export_buffer;
    int* _import_buffer;
    int _max_import_buffer_bytes;

    pid_t _child_pid;
    SolvingStates::SolvingState _state = SolvingStates::INITIALIZING;

    std::list<std::pair<std::vector<int>, Checksum>> _temp_clause_buffers;

public:
    HordeProcessAdapter(const Parameters& params, HordeConfig&& config,
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
    std::vector<int> getCollectedClauses(Checksum& checksum);
    void digestClauses(const std::vector<int>& clauses, const Checksum& checksum);

    void dumpStats();
    
    bool check();
    std::pair<SatResult, std::vector<int>> getSolution();

    void freeSharedMemory();

private:
    void doDigest(const std::vector<int>& clauses, const Checksum& checksum);
    void initSharedMemory(HordeConfig&& config);
    void* createSharedMemoryBlock(std::string shmemSubId, size_t size, void* data);

};

#endif
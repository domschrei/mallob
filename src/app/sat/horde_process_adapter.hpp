
#ifndef DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H
#define DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H

#include <list>

#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "util/params.hpp"
#include "hordesat/solvers/solving_state.hpp"
#include "horde_shared_memory.hpp"
#include "data/checksum.hpp"
#include "util/sys/background_worker.hpp"

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
    std::list<std::pair<std::vector<int>, Checksum>> _temp_clause_buffers;

    pid_t _child_pid = -1;
    SolvingStates::SolvingState _state = SolvingStates::INITIALIZING;

    std::atomic_int _written_revision = -1;
    int _published_revision = 0;
    int _desired_revision = -1;

    BackgroundWorker _background_worker;
    std::list<RevisionData> _revisions_to_write;
    Mutex _revisions_mutex;
    Mutex _state_mutex;

public:
    HordeProcessAdapter(Parameters&& params, HordeConfig&& config,
            size_t fSize, const int* fLits, size_t aSize, const int* aLits);
    ~HordeProcessAdapter();

    void run();
    bool isFullyInitialized();

    void appendRevisions(const std::vector<RevisionData>& revisions, int desiredRevision);

    void setSolvingState(SolvingStates::SolvingState state);
    void applySolvingState();

    void collectClauses(int maxSize);
    bool hasCollectedClauses();
    std::vector<int> getCollectedClauses(Checksum& checksum);
    void digestClauses(const std::vector<int>& clauses, const Checksum& checksum);

    void dumpStats();
    
    bool check();
    std::pair<SatResult, std::vector<int>> getSolution();

    void waitUntilChildExited();
    void freeSharedMemory();

private:
    void doDigest(const std::vector<int>& clauses, const Checksum& checksum);
    void initSharedMemory(HordeConfig&& config);
    void* createSharedMemoryBlock(std::string shmemSubId, size_t size, void* data);

};

#endif
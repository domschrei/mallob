
#ifndef DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H
#define DOMPASCH_MALLOB_HORDE_PROCESS_ADAPTER_H

#include <list>
#include <future>

#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "util/params.hpp"
#include "hordesat/solvers/solving_state.hpp"
#include "horde_shared_memory.hpp"
#include "data/checksum.hpp"
#include "util/sys/background_worker.hpp"
#include "data/job_result.hpp"

class ForkedSatJob; // fwd
class AnytimeSatClauseCommunicator;

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
    HordeConfig _config;
    std::shared_ptr<Logger> _log;

    ForkedSatJob* _job;
    AnytimeSatClauseCommunicator* _clause_comm = nullptr;

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

    volatile bool _running = false;
    volatile bool _initialized = false;
    volatile bool _terminate = false;
    volatile bool _bg_writer_running = false;

    std::future<void> _bg_initializer;
    std::future<void> _bg_writer;

    int* _export_buffer;
    int* _import_buffer;
    int* _returned_buffer;
    std::list<std::pair<std::vector<int>, Checksum>> _temp_clause_buffers;
    std::list<std::vector<int>> _temp_returned_clauses;

    pid_t _child_pid = -1;
    SolvingStates::SolvingState _state = SolvingStates::INITIALIZING;

    std::atomic_int _written_revision = 0;
    int _published_revision = 0;
    int _desired_revision = -1;

    std::atomic_int _num_revisions_to_write = 0;
    std::list<RevisionData> _revisions_to_write;
    Mutex _revisions_mutex;
    Mutex _state_mutex;

    bool _solution_in_preparation = false;
    int _solution_revision_in_preparation = -1;
    JobResult _solution;
    std::future<void> _solution_prepare_future;

public:
    HordeProcessAdapter(Parameters&& params, HordeConfig&& config, ForkedSatJob* job, 
        size_t fSize, const int* fLits, size_t aSize, const int* aLits,
        AnytimeSatClauseCommunicator* comm = nullptr);
    ~HordeProcessAdapter();

    void run();
    bool isFullyInitialized();
    void appendRevisions(const std::vector<RevisionData>& revisions, int desiredRevision);

    void setSolvingState(SolvingStates::SolvingState state);

    bool hasClauseComm();
    AnytimeSatClauseCommunicator* getClauseComm() {return _clause_comm;}
    void releaseClauseComm() {_clause_comm = nullptr;}

    void collectClauses(int maxSize);
    bool hasCollectedClauses();
    std::vector<int> getCollectedClauses(Checksum& checksum);
    void digestClauses(const std::vector<int>& clauses, const Checksum& checksum);
    void returnClauses(const std::vector<int>& clauses);

    void dumpStats();
    
    enum SubprocessStatus {NORMAL, FOUND_RESULT, CRASHED};
    SubprocessStatus check();
    JobResult& getSolution();

    void waitUntilChildExited();
    void freeSharedMemory();

private:
    void doInitialize();
    void doWriteRevisions();
    void doPrepareSolution();
    
    void applySolvingState();
    void doDigest(const std::vector<int>& clauses, const Checksum& checksum);
    void doReturnClauses(const std::vector<int>& clauses);
    void initSharedMemory(HordeConfig&& config);
    void* createSharedMemoryBlock(std::string shmemSubId, size_t size, void* data);

};

#endif
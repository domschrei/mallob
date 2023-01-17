
#pragma once

#include <list>
#include <future>

#include "util/logger.hpp"
#include "util/robin_hood.hpp"
#include "util/sys/threading.hpp"
#include "util/params.hpp"
#include "../execution/solving_state.hpp"
#include "sat_shared_memory.hpp"
#include "data/checksum.hpp"
#include "util/sys/background_worker.hpp"
#include "data/job_result.hpp"

class ForkedSatJob; // fwd
class AnytimeSatClauseCommunicator;

class SatProcessAdapter {

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
    SatProcessConfig _config;
    std::shared_ptr<Logger> _log;

    ForkedSatJob* _job;
    std::shared_ptr<AnytimeSatClauseCommunicator> _clause_comm;

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
    SatSharedMemory* _hsm = nullptr;

    volatile bool _running = false;
    volatile bool _initialized = false;
    volatile bool _terminate = false;
    volatile bool _bg_writer_running = false;

    std::future<void> _bg_initializer;
    std::future<void> _bg_writer;

    int* _export_buffer;
    int* _import_buffer;
    int* _filter_buffer;
    int* _returned_buffer;

    struct BufferTask {
        enum Type {
            FILTER_CLAUSES, APPLY_FILTER, DIGEST_CLAUSES_WITHOUT_FILTER, RETURN_CLAUSES, DIGEST_HISTORIC_CLAUSES
        } type;
        std::vector<int> payload;
        int epoch;
        int epochEnd;
    };
    std::list<BufferTask> _pending_tasks;
    std::pair<int, int> _last_admitted_clause_share;
    robin_hood::unordered_flat_set<int> _epochs_to_filter;
    int _epoch_of_export_buffer {-1};

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
    SatProcessAdapter(Parameters&& params, SatProcessConfig&& config, ForkedSatJob* job, 
        size_t fSize, const int* fLits, size_t aSize, const int* aLits,
        std::shared_ptr<AnytimeSatClauseCommunicator>& comm);
    ~SatProcessAdapter();

    void run();
    bool isFullyInitialized();
    void appendRevisions(const std::vector<RevisionData>& revisions, int desiredRevision);
    void crash();

    void setSolvingState(SolvingStates::SolvingState state);

    int getStartedNumThreads() const {return _config.threads;}

    void collectClauses(int maxSize);
    bool hasCollectedClauses();
    std::vector<int> getCollectedClauses();
    std::pair<int, int> getLastAdmittedClauseShare();

    void filterClauses(int epoch, const std::vector<int>& clauses);
    bool hasFilteredClauses(int epoch);
    std::vector<int> getLocalFilter(int epoch);
    void applyFilter(int epoch, const std::vector<int>& filter);

    void digestClausesWithoutFilter(const std::vector<int>& clauses);
    void returnClauses(const std::vector<int>& clauses);
    void digestHistoricClauses(int epochBegin, int epochEnd, const std::vector<int>& clauses);

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

    void tryProcessNextTasks();
    bool process(BufferTask& task);
    
    void applySolvingState();
    void initSharedMemory(SatProcessConfig&& config);
    void* createSharedMemoryBlock(std::string shmemSubId, size_t size, void* data);

};

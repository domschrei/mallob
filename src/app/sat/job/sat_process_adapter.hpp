
#pragma once

#include <sys/types.h>
#include <list>
#include <future>
#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "robin_map.h"
#include "util/logger.hpp"
#include "util/robin_hood.hpp"
#include "util/sys/bidirectional_anytime_pipe.hpp"
#include "util/sys/bidirectional_anytime_pipe_shmem.hpp"
#include "util/sys/threading.hpp"
#include "util/params.hpp"
#include "../execution/solving_state.hpp"
#include "sat_shared_memory.hpp"
#include "data/checksum.hpp"
#include "util/sys/background_worker.hpp"
#include "data/job_result.hpp"
#include "app/sat/job/sat_process_config.hpp"
#include "util/hashing.hpp"

class ForkedSatJob; // fwd
class AnytimeSatClauseCommunicator;
class BiDirectionalPipe;
class Logger;
struct SatSharedMemory;

class SatProcessAdapter {

public:
    struct RevisionData {
        int revision;
        Checksum checksum;
        size_t fSize;
        const int* fLits;
        size_t aSize;
        const int* aLits;
        int descriptionId;
    };
    struct ShmemObject {
        std::string id; 
        void* data {nullptr}; 
        size_t size;
        bool managedInCache {false};
        int revision {0};
        std::string userLabel;
        int descId {0};
        bool operator==(const ShmemObject& other) const {
            return id == other.id && size == other.size;
        }
    };

private:
    Parameters _params;
    SatProcessConfig _config;
    std::shared_ptr<Logger> _log;

    std::shared_ptr<AnytimeSatClauseCommunicator> _clause_comm;

    size_t _f_size;
    const int* _f_lits;
    size_t _a_size;
    const int* _a_lits;
    int _desc_id;

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
    Mutex _mtx_preregistered_shmem;
    robin_hood::unordered_flat_map<std::string, ShmemObject> _preregistered_shmem;

    Mutex _mtx_pipe;
    std::unique_ptr<BiDirectionalAnytimePipeShmem> _pipe;

    volatile bool _running = false;
    volatile bool _initialized = false;
    volatile bool _bg_writer_running = false;
    std::atomic_bool _terminate {false};
    volatile bool _destructed {false};

    std::future<void> _bg_initializer;
    std::future<void> _bg_writer;

    int _last_admitted_nb_lits {0};
    int _successful_solver_id {-1};
    int _nb_incoming_lits {0};
    enum ClauseCollectingStage {NONE, QUERIED, RETURNED} _clause_collecting_stage {NONE};
    std::vector<int> _collected_clauses;
    tsl::robin_map<int, std::vector<int>> _filters_by_epoch;
    int _epoch_of_export_buffer {-1};
    long long _best_found_objective_cost {LLONG_MAX};

    pid_t _child_pid = -1;
    SolvingStates::SolvingState _state = SolvingStates::INITIALIZING;

    std::atomic_int _written_revision = 0;
    int _published_revision = 0;
    int _desired_revision = -1;
    int _clause_buffer_revision = -1;

    std::atomic_int _num_revisions_to_write = 0;
    std::list<RevisionData> _revisions_to_write;
    Mutex _mtx_revisions;
    Mutex _mtx_state;
    unsigned long _sum_of_revision_sizes {0};

    bool _thread_count_update {false};
    int _nb_threads {0};

    JobResult _solution;

public:
    SatProcessAdapter(Parameters&& params, SatProcessConfig&& config,
        size_t fSize, const int* fLits, size_t aSize, const int* aLits,
        int descId, std::shared_ptr<AnytimeSatClauseCommunicator>& comm);
    ~SatProcessAdapter();

    void run();
    bool isFullyInitialized();
    void appendRevisions(const std::vector<RevisionData>& revisions, int desiredRevision, int nbThreads);
    void setDesiredRevision(int desiredRevision) {_desired_revision = desiredRevision;}
    void preregisterShmemObject(ShmemObject&& obj);
    void crash();
    void reduceThreadCount();

    void setSolvingState(SolvingStates::SolvingState state);
    void setClauseBufferRevision(int revision) {_clause_buffer_revision = std::max(_clause_buffer_revision, revision);}
    void updateBestFoundSolutionCost(long long bestFoundSolutionCost);

    int getStartedNumThreads() const {return _config.threads;}

    void collectClauses(int maxSize);
    bool hasCollectedClauses();
    std::vector<int> getCollectedClauses(int& successfulSolverId, int& numLits);
    int getLastAdmittedNumLits();
    long long getBestFoundObjectiveCost() const;

    void filterClauses(int epoch, std::vector<int>&& clauses);
    bool hasFilteredClauses(int epoch);
    std::vector<int> getLocalFilter(int epoch);
    void applyFilter(int epoch, std::vector<int>&& filter);
    void digestClausesWithoutFilter(int epoch, std::vector<int>&& clauses, bool stateless);

    void returnClauses(std::vector<int>&& clauses);
    void digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>&& clauses);

    void dumpStats();
    
    enum SubprocessStatus {NORMAL, FOUND_RESULT, CRASHED};
    SubprocessStatus check();
    JobResult& getSolution();

    void waitUntilChildExited();
    void freeSharedMemory();

private:
    void doInitialize();
    void doWriteRevisions();
    void doTerminateInitializedProcess();
    
    void applySolvingState();
    void initSharedMemory(SatProcessConfig&& config);
    void* createSharedMemoryBlock(std::string shmemSubId, size_t size, const void* data, int rev = 0, int descId = -1, bool managedInCache = false);

};

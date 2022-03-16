
#ifndef DOMPASCH_MALLOB_ANYTIME_SAT_CLAUSE_COMMUNICATOR_H
#define DOMPASCH_MALLOB_ANYTIME_SAT_CLAUSE_COMMUNICATOR_H

#include <future>

#include "util/params.hpp"
#include "util/hashing.hpp"
#include "hordesat/sharing/adaptive_clause_database.hpp"
#include "data/job_transfer.hpp"
#include "app/job.hpp"
#include "base_sat_job.hpp"
#include "clause_history.hpp"
#include "distributed_clause_filter.hpp"

class AnytimeSatClauseCommunicator {

public:
    enum Stage {
        PREPARING_CLAUSES, 
        MERGING, 
        WAITING_FOR_CLAUSE_BCAST, 
        PREPARING_FILTER, 
        WAITING_FOR_FILTER_BCAST
    };

private:
    const Parameters& _params;
    BaseSatJob* _job = NULL;
    bool _suspended = false;

    const int _clause_buf_base_size;
    const float _clause_buf_discount_factor;
    const bool _use_cls_history;

    AdaptiveClauseDatabase _cdb;
    ClauseHistory _cls_history;
    DistributedClauseFilter _filter;

    struct Session {

        const Parameters& _params;
        BaseSatJob* _job;
        AdaptiveClauseDatabase& _cdb;
        DistributedClauseFilter& _filter;
        int _epoch;
        Stage _stage = PREPARING_CLAUSES;

        int _num_aggregated_nodes = 0;
        std::list<std::vector<int>> _clause_buffers;
        int _desired_num_child_buffers;

        std::list<std::vector<int>> _clause_buffers_being_merged;
        std::vector<int> _excess_clauses_from_merge;
        bool _is_done_merging = false;
        std::future<void> _merge_future;
        
        std::vector<int> _clause_buffer_being_filtered;
        std::vector<int> _local_filter_bitset;
        bool _is_done_filtering = false;
        std::future<void> _filter_future;

        std::vector<int> _aggregated_filter_bitset;
        int _num_aggregated_filters = 0;



        Session(const Parameters& params, BaseSatJob* job, AdaptiveClauseDatabase& cdb, DistributedClauseFilter& filter, int epoch) : 
            _params(params), _job(job), _cdb(cdb), _filter(filter), _epoch(epoch) {}

        void storePreparedClauseBuffer();

        void initiateMergeOfClauseBuffers();
        std::vector<int> getMergedClauseBuffer();
        void publishMergedClauses();

        void processBroadcastClauses(std::vector<int>& clauses);

        void addFilter(std::vector<int>& filter);
        void publishLocalAggregatedFilter();
        std::vector<int> applyGlobalFilter(const std::vector<int>& filter, std::vector<int>& clauses);

        std::vector<int> merge(size_t maxSize);

        bool isDestructible() {
            if (_merge_future.valid() && !_is_done_merging) return false;
            if (_filter_future.valid() && !_is_done_filtering) return false;
            return true;
        }

        ~Session() {
            if (_merge_future.valid()) _merge_future.get();
            if (_filter_future.valid()) _filter_future.get();
        }
    };

    std::list<Session> _sessions;

    int _current_epoch = 0;
    float _time_of_last_epoch_initiation = 0;
    float _time_of_last_epoch_conclusion = 0.1;

public:
    AnytimeSatClauseCommunicator(const Parameters& params, BaseSatJob* job) : _params(params), _job(job), 
        _clause_buf_base_size(_params.clauseBufferBaseSize()), 
        _clause_buf_discount_factor(_params.clauseBufferDiscountFactor()),
        _use_cls_history(params.collectClauseHistory()),
        _cdb([&]() {
            AdaptiveClauseDatabase::Setup setup;
            setup.maxClauseLength = _params.strictClauseLengthLimit();
            setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
            setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
            setup.numLiterals = 0;
            setup.numProducers = 0;
            return setup;
        }()),
        _cls_history(_params, _job->getBufferLimit(_job->getJobTree().getCommSize(), MyMpi::ALL), *job, _cdb),
        _filter(params.clauseFilterClearInterval()) {

        _time_of_last_epoch_initiation = Timer::elapsedSeconds();
        _time_of_last_epoch_conclusion = Timer::elapsedSeconds();
    }

    ~AnytimeSatClauseCommunicator() {}

    void communicate();
    void handle(int source, JobMessage& msg);
    void feedHistoryIntoSolver();
    bool isDestructible() {
        for (auto& session : _sessions) if (!session.isDestructible()) return false;
        return true;
    }

private:
    inline Session& currentSession() {return _sessions.back();}
    void learnClauses(std::vector<int>& clauses, int epoch, bool writeIntoClauseHistory);
};

#endif
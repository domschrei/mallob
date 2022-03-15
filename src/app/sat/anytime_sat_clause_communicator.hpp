
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
        IDLE, 
        PREPARING_CLAUSES, 
        MERGING, 
        WAITING_FOR_CLAUSE_BCAST, 
        PREPARING_FILTER, 
        WAITING_FOR_FILTER_BCAST
    };

private:
    Parameters _params;
    BaseSatJob* _job = NULL;
    bool _suspended = false;

    const int _clause_buf_base_size;
    const float _clause_buf_discount_factor;
    const bool _use_checksums;
    const bool _use_cls_history;

    AdaptiveClauseDatabase _cdb;
    ClauseHistory _cls_history;
    DistributedClauseFilter _filter;

    Stage _stage = IDLE;

    std::list<std::vector<int>> _clause_buffers;

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

    int _num_aggregated_nodes;
    int _current_epoch = 0;
    float _time_of_last_epoch_conclusion = 0;

public:
    AnytimeSatClauseCommunicator(const Parameters& params, BaseSatJob* job) : _params(params), _job(job), 
        _clause_buf_base_size(_params.clauseBufferBaseSize()), 
        _clause_buf_discount_factor(_params.clauseBufferDiscountFactor()),
        _use_checksums(params.useChecksums()),
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
        _cls_history(_params, getBufferLimit(_job->getJobTree().getCommSize(), MyMpi::ALL), *job, _cdb),
        _filter(params.clauseFilterClearInterval()),
        _num_aggregated_nodes(0) {

        _time_of_last_epoch_conclusion = Timer::elapsedSeconds();
    }

    ~AnytimeSatClauseCommunicator() {
        if (_merge_future.valid()) _merge_future.get();
        if (_filter_future.valid()) _filter_future.get();
    }

    void communicate();
    void handle(int source, JobMessage& msg);
    void feedHistoryIntoSolver();

private:
    size_t getBufferLimit(int numAggregatedNodes, MyMpi::BufferQueryMode mode);

    void publishMergedClauses();
    void initiateMergeOfClauseBuffers();
    std::vector<int> getMergedClauseBuffer();

    void addFilter(std::vector<int>& filter);
    void publishAggregatedFilter();
    std::vector<int> applyFilter(const std::vector<int>& filter, std::vector<int>& clauses);

    void broadcastAndProcess(std::vector<int>& clauses);
    void learnClauses(std::vector<int>& clauses);
    void sendClausesToChildren(std::vector<int>& clauses);

    std::vector<int> merge(size_t maxSize);

    std::vector<int> getEmptyBuffer() {
        return std::vector<int>();
    }
};

#endif
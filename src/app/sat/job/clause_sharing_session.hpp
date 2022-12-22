
#pragma once

#include "comm/msgtags.h"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "base_sat_job.hpp"
#include "app/sat/sharing/buffer/adaptive_clause_database.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "clause_history.hpp"

class ClauseSharingSession {

private:
    const Parameters& _params;
    BaseSatJob* _job;
    AdaptiveClauseDatabase& _cdb;
    ClauseHistory* _cls_history;
    int _epoch;
    enum Stage {
        PRODUCING_CLAUSES,
        AGGREGATING_CLAUSES,
        PRODUCING_FILTER,
        AGGREGATING_FILTER,
        DONE
    } _stage {PRODUCING_CLAUSES};

    std::vector<int> _excess_clauses_from_merge;
    std::vector<int> _broadcast_clause_buffer;
    int _num_broadcast_clauses;
    int _num_admitted_clauses;

    JobTreeAllReduction _allreduce_clauses;
    JobTreeAllReduction _allreduce_filter;

public:
    ClauseSharingSession(const Parameters& params, BaseSatJob* job, AdaptiveClauseDatabase& cdb,
            ClauseHistory* clsHistory, int epoch, float compensationFactor) : 
        _params(params), _job(job), _cdb(cdb), _cls_history(clsHistory), _epoch(epoch),
        _allreduce_clauses(
            job->getJobTree(),
            // Base message 
            JobMessage(_job->getId(), _job->getContextId(), _job->getRevision(), epoch, MSG_ALLREDUCE_CLAUSES),
            // Neutral element
            std::vector<int>(1, 1), // only integer: number of aggregated job tree nodes
            // Aggregator for local + incoming elements
            [&](std::list<std::vector<int>>& elems) {
                return mergeClauseBuffersDuringAggregation(elems);
            }
        ),
        _allreduce_filter(
            job->getJobTree(), 
            // Base message
            JobMessage(_job->getId(), _job->getContextId(), _job->getRevision(), epoch, MSG_ALLREDUCE_FILTER),
            // Neutral element
            std::vector<int>(ClauseMetadata::numBytes(), 0),
            // Aggregator for local + incoming elements
            [&](std::list<std::vector<int>>& elems) {
                return mergeFiltersDuringAggregation(elems);
            }
        ) {

        LOG(V4_VVER, "%s CS OPEN e=%i\n", _job->toStr(), _epoch);

        _job->setSharingCompensationFactor(compensationFactor);

        if (!_job->hasPreparedSharing()) {
            int limit = _job->getBufferLimit(1, MyMpi::SELF);
            _job->prepareSharing(limit);
        }
    }

    void pruneChild(int rank) {
        _allreduce_clauses.pruneChild(rank);
        _allreduce_filter.pruneChild(rank);
    }

    void advanceSharing() {

        if (_stage == PRODUCING_CLAUSES && _job->hasPreparedSharing()) {

            // Produce contribution to all-reduction of clauses
            LOG(V4_VVER, "%s CS produced cls\n", _job->toStr());
            _allreduce_clauses.produce([&]() {
                Checksum checksum;
                auto clauses = _job->getPreparedClauses(checksum);
                clauses.push_back(1); // # aggregated workers
                return clauses;
            });

            _stage = AGGREGATING_CLAUSES;
        }

        if (_stage == AGGREGATING_CLAUSES && _allreduce_clauses.advance().hasResult()) {

            LOG(V4_VVER, "%s CS filter\n", _job->toStr());

            // Some clauses may have been left behind during merge
            if (_excess_clauses_from_merge.size() > sizeof(size_t)/sizeof(int)) {
                // Add them as produced clauses to your local solver
                // so that they can be re-exported (if they are good enough)
                _job->returnClauses(_excess_clauses_from_merge);
            }

            // Fetch initial clause buffer (result of all-reduction of clauses)
            _broadcast_clause_buffer = _allreduce_clauses.extractResult();

            // Initiate production of local filter element for 2nd all-reduction 
            _job->filterSharing(_epoch, _broadcast_clause_buffer);
            _stage = PRODUCING_FILTER;
        }

        if (_stage == PRODUCING_FILTER && _job->hasFilteredSharing(_epoch)) {

            LOG(V4_VVER, "%s CS produced filter\n", _job->toStr());
            _allreduce_filter.produce([&]() {return _job->getLocalFilter(_epoch);});
            _stage = AGGREGATING_FILTER;
        }

        if (_stage == AGGREGATING_FILTER && _allreduce_filter.advance().hasResult()) {

            LOG(V4_VVER, "%s CS apply filter\n", _job->toStr());

            // Extract and digest result
            auto filter = _allreduce_filter.extractResult();
            _job->applyFilter(_epoch, filter);
            if (_params.collectClauseHistory()) {
                auto filteredClauses = applyGlobalFilter(filter, _broadcast_clause_buffer);
                addToClauseHistory(filteredClauses, _epoch);
            }

            // Conclude this sharing epoch
            _stage = DONE;
        }
    }

    bool advanceClauseAggregation(int source, int mpiTag, JobMessage& msg) {
        bool success = false;
        if (msg.tag == MSG_ALLREDUCE_CLAUSES && _allreduce_clauses.isValid()) {
            success = _allreduce_clauses.receive(source, mpiTag, msg);
            advanceSharing();
        }
        return success;
    }
    bool advanceFilterAggregation(int source, int mpiTag, JobMessage& msg) {
        bool success = false;
        if (msg.tag == MSG_ALLREDUCE_FILTER && _allreduce_filter.isValid()) {
            success = _allreduce_filter.receive(source, mpiTag, msg);
            advanceSharing();
        }
        return success;
    }

    bool isDone() const {
        return _stage == DONE;
    }

    bool isDestructible() {
        return _allreduce_clauses.isDestructible() && _allreduce_filter.isDestructible();
    }

    ~ClauseSharingSession() {
        LOG(V4_VVER, "%s CS CLOSE e=%i\n", _job->toStr(), _epoch);
        // If not done producing, will send empty clause buffer upwards
        _allreduce_clauses.cancel();
        // If not done producing, will send empty filter upwards
        _allreduce_filter.cancel();
    }

private:
    std::vector<int> applyGlobalFilter(const std::vector<int>& filter, std::vector<int>& clauses);
    
    std::vector<int> mergeClauseBuffersDuringAggregation(std::list<std::vector<int>>& elems) {
        int numAggregated = 0;
        for (auto& elem : elems) {
            numAggregated += elem.back();
            elem.pop_back();
        }
        auto merger = _cdb.getBufferMerger(_job->getBufferLimit(numAggregated, MyMpi::ALL));
        for (auto& elem : elems) {
            merger.add(_cdb.getBufferReader(elem.data(), elem.size()));
        }
        std::vector<int> merged = merger.merge(&_excess_clauses_from_merge);
        LOG(V4_VVER, "%s : merged %i contribs ~> len=%i\n", 
            _job->toStr(), numAggregated, merged.size());
        merged.push_back(numAggregated);
        return merged;
    }

    std::vector<int> mergeFiltersDuringAggregation(std::list<std::vector<int>>& elems) {
        std::vector<int> filter = std::move(elems.front());
        elems.pop_front();

        unsigned long maxMinEpochId;
        if (ClauseMetadata::enabled()) {
            assert(filter.size() >= 2);
            maxMinEpochId = ClauseMetadata::readUnsignedLong(filter.data());
        }

        for (auto& elem : elems) {
            if (filter.size() < elem.size()) 
                filter.resize(elem.size());
            if (ClauseMetadata::enabled()) {
                assert(elem.size() >= 2);
                unsigned long minEpochId = ClauseMetadata::readUnsignedLong(elem.data());
                maxMinEpochId = std::max(maxMinEpochId, minEpochId);
            }

            for (size_t i = ClauseMetadata::numBytes(); i < elem.size(); i++) {
                filter[i] |= elem[i]; // bitwise OR
            }
        }

        if (ClauseMetadata::enabled()) {
            ClauseMetadata::writeUnsignedLong(maxMinEpochId, filter.data());
        }

        return filter;
    }

    void addToClauseHistory(std::vector<int>& clauses, int epoch) {
        LOG(V4_VVER, "%s : learn s=%i\n", _job->toStr(), clauses.size());
        
        // Add clause batch to history
        _cls_history->addEpoch(epoch, clauses, /*entireIndex=*/false);

        // Send next batches of historic clauses to subscribers as necessary
        _cls_history->sendNextBatches();
    }
};


#pragma once

#include "comm/msgtags.h"
#include "data/job_transfer.hpp"
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
    ClauseHistory& _cls_history;
    int _epoch;

    std::vector<int> _excess_clauses_from_merge;
    std::vector<int> _broadcast_clause_buffer;
    int _num_broadcast_clauses;
    int _num_admitted_clauses;

    JobTreeAllReduction _allreduce_clauses;
    JobTreeAllReduction _allreduce_filter;
    bool _filtering = false;
    bool _concluded = false;

    float _compensation_decay {0.6};

public:
    ClauseSharingSession(const Parameters& params, BaseSatJob* job, AdaptiveClauseDatabase& cdb,
            ClauseHistory& clsHistory, int epoch) : 
        _params(params), _job(job), _cdb(cdb), _cls_history(clsHistory), _epoch(epoch),
        _allreduce_clauses(
            job->getJobTree(),
            // Base message 
            JobMessage(_job->getId(), _job->getRevision(), epoch, MSG_ALLREDUCE_CLAUSES),
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
            JobMessage(_job->getId(), _job->getRevision(), epoch, MSG_ALLREDUCE_FILTER),
            // Neutral element
            std::vector<int>(),
            // Aggregator for local + incoming elements
            [&](std::list<std::vector<int>>& elems) {
                return mergeFiltersDuringAggregation(elems);
            }
        ) { }

    ~ClauseSharingSession() {
        _allreduce_clauses.destroy();
        _allreduce_filter.destroy();
    }

    void advanceSharing() {

        if (!_allreduce_clauses.hasProducer() && _job->hasPreparedSharing()) {

            // Produce contribution to all-reduction of clauses
            LOG(V4_VVER, "%s CS produce cls\n", _job->toStr());
            _allreduce_clauses.produce([&]() {
                Checksum checksum;
                auto clauses = _job->getPreparedClauses(checksum);
                clauses.push_back(1); // # aggregated workers
                return clauses;
            });
        
            // Calculate new sharing compensation factor from last sharing statistics
            auto [nbAdmitted, nbBroadcast] = _job->getLastAdmittedClauseShare();
            float admittedRatio = nbBroadcast == 0 ? 1 : ((float)nbAdmitted) / nbBroadcast;
            admittedRatio = std::max(0.01f, admittedRatio);
            float newCompensationFactor = std::max(1.f, std::min(
                (float)_params.clauseHistoryAggregationFactor(), 1.f/admittedRatio
            ));
            float compensationFactor = _job->getCompensationFactor();
            compensationFactor = _compensation_decay * compensationFactor + (1-_compensation_decay) * newCompensationFactor;
            _job->setSharingCompensationFactor(compensationFactor);
            if (_job->getJobTree().isRoot()) {
                LOG(V3_VERB, "%s CS last sharing: %i/%i globally passed ~> c=%.3f\n", _job->toStr(), 
                    nbAdmitted, nbBroadcast, compensationFactor);       
            }
        
        } else if (!_allreduce_clauses.hasProducer()) {
            // No sharing prepared yet: Retry
            _job->prepareSharing(_job->getBufferLimit(1, MyMpi::SELF));
        }

        _allreduce_clauses.advance();

        // All-reduction of clauses finished?
        if (_allreduce_clauses.hasResult()) {

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
            _job->filterSharing(_broadcast_clause_buffer);
        }

        // Supply calculated local filter to the 2nd all-reduction
        if (!_allreduce_filter.hasProducer() && _job->hasFilteredSharing()) {
            LOG(V4_VVER, "%s CS produce filter\n", _job->toStr());
            _allreduce_filter.produce([&]() {return _job->getLocalFilter();});
        }

        // Advance all-reduction of filter
        _allreduce_filter.advance();

        // All-reduction of clause filter finished?
        if (_allreduce_filter.hasResult()) {
            
            LOG(V4_VVER, "%s CS apply filter\n", _job->toStr());

            // Extract and digest result
            auto filter = _allreduce_filter.extractResult();
            _job->applyFilter(filter);
            if (_params.collectClauseHistory()) {
                auto filteredClauses = applyGlobalFilter(filter, _broadcast_clause_buffer);
                addToClauseHistory(filteredClauses, _epoch);
            }

            // Conclude this sharing epoch
            _concluded = true;
        }
    }

    bool isConcluded() const {return _concluded;}

    bool advanceClauseAggregation(int source, int mpiTag, JobMessage& msg) {
        bool success = false;
        if (msg.tag == MSG_ALLREDUCE_CLAUSES && _allreduce_clauses.isValid()) {
            success = _allreduce_clauses.receive(source, mpiTag, msg);
            _allreduce_clauses.advance();
        }
        return success;
    }
    bool advanceFilterAggregation(int source, int mpiTag, JobMessage& msg) {
        bool success = false;
        if (msg.tag == MSG_ALLREDUCE_FILTER && _allreduce_filter.isValid()) {
            success = _allreduce_filter.receive(source, mpiTag, msg);
            _allreduce_filter.advance();
        }
        return success;
    }

    std::vector<int> applyGlobalFilter(const std::vector<int>& filter, std::vector<int>& clauses);

    void cancel() {
        _allreduce_clauses.cancel();
        _allreduce_filter.cancel();
    }

    bool isValid() const {
        return _allreduce_clauses.isValid() || _allreduce_filter.isValid();
    }

    bool isDestructible() {
        return _allreduce_clauses.isDestructible() && _allreduce_filter.isDestructible();
    }

private:
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
        for (auto& elem : elems) {
            if (filter.size() < elem.size()) 
                filter.resize(elem.size());
            for (size_t i = 0; i < elem.size(); i++) {
                filter[i] |= elem[i]; // bitwise OR
            }
        }
        return filter;
    }

    void addToClauseHistory(std::vector<int>& clauses, int epoch) {
        LOG(V4_VVER, "%s : learn s=%i\n", _job->toStr(), clauses.size());
        
        // Add clause batch to history
        _cls_history.addEpoch(epoch, clauses, /*entireIndex=*/false);

        // Send next batches of historic clauses to subscribers as necessary
        _cls_history.sendNextBatches();
    }

};


#pragma once

#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/job/clause_sharing_actor.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/filter/clause_buffer_lbd_scrambler.hpp"
#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/sharing/store/static_clause_store.hpp"
#include "comm/job_tree_snapshot.hpp"
#include "comm/msgtags.h"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "comm/job_tree_basic_all_reduction.hpp"
#include "historic_clause_storage.hpp"
#include "app/sat/sharing/filter/in_place_clause_filtering.hpp"
#include "util/random.hpp"
#include "inplace_sharing_aggregation.hpp"
#include <cstdint>

class ClauseSharingSession {

private:
    const Parameters& _params;
    ClauseSharingActor* _job;
    HistoricClauseStorage* _cls_history;
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
    int _local_export_limit;
    int _num_broadcast_clauses;
    int _num_admitted_clauses;
    long long _best_found_solution_cost;

    JobTreeBasicAllReduction _allreduce_clauses;
    std::optional<JobTreeBasicAllReduction> _allreduce_filter;

    SplitMix64Rng _rng;

    bool _has_clause_listener {false};
    std::function<void(std::vector<int>&)> _clause_listener;

    std::unique_ptr<StaticClauseStore<false>> _merge_store;
    bool _priority_based_buffer_merging = false;

public:
    ClauseSharingSession(const Parameters& params, ClauseSharingActor* actor, const JobTreeSnapshot& snapshot,
            HistoricClauseStorage* clsHistory, int epoch, float compensationFactor) : 
        _params(params), _job(actor), _cls_history(clsHistory), _epoch(epoch),
        _allreduce_clauses(
            snapshot,
            // Base message 
            JobMessage(_job->getActorJobId(), _job->getActorContextId(), _job->getClausesRevision(), epoch, MSG_ALLREDUCE_CLAUSES),
            // Neutral element
            InplaceClauseAggregation::neutralElem(),
            // Aggregator for local + incoming elements
            [&](std::list<std::vector<int>>& elems) {
                return mergeClauseBuffersDuringAggregation(elems);
            }
        ), _rng(_params.seed()+69) {

        if (_params.clauseFilterMode() == MALLOB_CLAUSE_FILTER_EXACT_DISTRIBUTED) {
            _allreduce_filter.emplace(
                snapshot, 
                // Base message
                JobMessage(_job->getActorJobId(), _job->getActorContextId(), _job->getClausesRevision(), epoch, MSG_ALLREDUCE_FILTER),
                // Neutral element
                std::vector<int>(ClauseMetadata::enabled() ? 2 : 0, 0),
                // Aggregator for local + incoming elements
                [&](std::list<std::vector<int>>& elems) {
                    return mergeFiltersDuringAggregation(elems);
                }
            );
        }

        LOG(V5_DEBG, "%s CS OPEN e=%i\n", _job->getLabel(), _epoch);
        _local_export_limit = _job->setSharingCompensationFactorAndUpdateExportLimit(compensationFactor);
        if (!_job->hasPreparedSharing()) _job->prepareSharing();
    }

    void pruneChild(int rank) {
        _allreduce_clauses.pruneChild(rank);
        if (_allreduce_filter) _allreduce_filter->pruneChild(rank);
    }

    void setAdditionalClauseListener(std::function<void(std::vector<int>&)> cb) {
        _has_clause_listener = true;
        _clause_listener = cb;
    }

    void advanceSharing() {

        if (_stage == PRODUCING_CLAUSES && _job->hasPreparedSharing()) {

            // Produce contribution to all-reduction of clauses
            _allreduce_clauses.produce([&]() {
                Checksum checksum;
                int successfulSolverId;
                int numLits;
                auto clauses = _job->getPreparedClauses(checksum, successfulSolverId, numLits);
                LOG(V4_VVER, "%s CS produced cls size=%lu lits=%i/%i\n", _job->getLabel(), clauses.size(), numLits, _local_export_limit);
                auto agg = InplaceClauseAggregation::prepareRawBuffer(clauses,
                    _job->getClausesRevision(), numLits, 1, successfulSolverId,
                    _job->getBestFoundObjectiveCost());
                return clauses;
            });

            _stage = AGGREGATING_CLAUSES;
        }

        if (_stage == AGGREGATING_CLAUSES && _allreduce_clauses.advance().hasResult()) {

            // Some clauses may have been left behind during merge
            if (_excess_clauses_from_merge.size() > 4) {
                // Add them as produced clauses to your local solver
                // so that they can be re-exported (if they are good enough)
                _job->returnClauses(std::move(_excess_clauses_from_merge));
            }

            // Fetch initial clause buffer (result of all-reduction of clauses)
            _broadcast_clause_buffer = _allreduce_clauses.extractResult();
            auto aggregation = InplaceClauseAggregation(_broadcast_clause_buffer);
            _best_found_solution_cost = aggregation.bestFoundSolutionCost();
            // If desired, scramble the LBD scores of featured clauses
            if (_params.scrambleLbdScores()) {
                float time = Timer::elapsedSeconds();
                // 1. Create reader for shared clause buffer
                initMergeClauseStore();
                BufferReader reader = _merge_store->getBufferReader(_broadcast_clause_buffer.data(),
                    _broadcast_clause_buffer.size() - aggregation.numMetadataInts());
                // 2. Scramble clauses within each clause length w.r.t. LBD scores
                ClauseBufferLbdScrambler scrambler(_params, reader);
                auto modifiedClauseBuffer = scrambler.scrambleLbdScores();
                // 3. Overwrite clause buffer within our aggregation buffer
                aggregation.replaceClauses(modifiedClauseBuffer);
                time = Timer::elapsedSeconds() - time;
                LOG(V4_VVER, "%s scrambled LBDs in %.4fs\n", _job->getLabel(), time);
            }
            int winningSolverId = aggregation.successfulSolver();
            assert(winningSolverId >= -1 || log_return_false("Winning solver ID = %i\n", winningSolverId));
            _job->setNumInputLitsOfLastSharing(aggregation.numInputLiterals());
            _job->setClauseBufferRevision(aggregation.maxRevision());
            _job->updateBestFoundSolutionCost(_best_found_solution_cost);

            if (_allreduce_filter) {
                // Initiate production of local filter element for 2nd all-reduction 
                LOG(V5_DEBG, "%s CS filter\n", _job->getLabel());
                _job->filterSharing(_epoch, std::vector<int>(_broadcast_clause_buffer));
                _stage = PRODUCING_FILTER;
            } else {
                // No distributed filtering: Sharing is done!
                LOG(V5_DEBG, "%s CS digest w/o filter\n", _job->getLabel());
                _job->digestSharingWithoutFilter(_epoch, std::vector<int>(_broadcast_clause_buffer), false);
                if (_cls_history) {
                    InplaceClauseAggregation(_broadcast_clause_buffer).stripToRawBuffer();
                    _cls_history->importSharing(_epoch, std::move(_broadcast_clause_buffer));
                }
                _stage = DONE;
            }
        }

        if (_stage == PRODUCING_FILTER && _job->hasFilteredSharing(_epoch)) {

            _allreduce_filter->produce([&]() {
                auto f = _job->getLocalFilter(_epoch);
                LOG(V5_DEBG, "%s CS produced filter, size %i\n", _job->getLabel(), f.size());
                return f;
            });
            _stage = AGGREGATING_FILTER;
        }

        if (_stage == AGGREGATING_FILTER && _allreduce_filter->advance().hasResult()) {


            // Extract and digest result
            auto filter = _allreduce_filter->extractResult();
            LOG(V5_DEBG, "%s CS digest w/ filter, size %i\n", _job->getLabel(), filter.size());
            if (_cls_history || _has_clause_listener) {
                InplaceClauseAggregation(_broadcast_clause_buffer).stripToRawBuffer();
                applyGlobalFilter(filter, _broadcast_clause_buffer);
                if (_cls_history) {
                    // Add clause batch to history
                    std::vector<int> copy(_broadcast_clause_buffer);
                    _cls_history->importSharing(_epoch, std::move(copy));
                }
                if (_has_clause_listener) _clause_listener(_broadcast_clause_buffer);
            }
            _job->applyFilter(_epoch, std::move(filter));

            // Conclude this sharing epoch
            _stage = DONE;
        }
    }

    bool advanceClauseAggregation(int source, int mpiTag, JobMessage& msg) {
        bool success = false;
        if (msg.contextIdOfDestination != _job->getActorContextId()) return success;
        if (msg.tag == MSG_ALLREDUCE_CLAUSES && _allreduce_clauses.isValid()) {
            success = _allreduce_clauses.receive(source, mpiTag, msg);
            advanceSharing();
        }
        return success;
    }
    bool advanceFilterAggregation(int source, int mpiTag, JobMessage& msg) {
        bool success = false;
        if (msg.contextIdOfDestination != _job->getActorContextId()) return success;
        if (msg.tag == MSG_ALLREDUCE_FILTER && _allreduce_filter->isValid()) {
            success = _allreduce_filter->receive(source, mpiTag, msg);
            advanceSharing();
        }
        return success;
    }

    bool isDone() const {
        return _stage == DONE;
    }

    bool isDestructible() {
        return _allreduce_clauses.isDestructible() && 
            (!_allreduce_filter || _allreduce_filter->isDestructible());
    }

    long long getBestFoundSolutionCost() const {
        return _best_found_solution_cost;
    }

    ~ClauseSharingSession() {
        LOG(V5_DEBG, "%s CS CLOSE e=%i\n", _job->getLabel(), _epoch);
        // If not done producing, will send empty clause buffer upwards
        _allreduce_clauses.cancel();
        // If not done producing, will send empty filter upwards
        if (_allreduce_filter) _allreduce_filter->cancel();
    }

private:
    void applyGlobalFilter(const std::vector<int>& filter, std::vector<int>& clauses) {
        
        InPlaceClauseFiltering filtering(_params, clauses, filter);
        int newSize = filtering.applyAndGetNewSize();
        clauses.resize(newSize);

        _num_broadcast_clauses = filtering.getNumClauses();
        _num_admitted_clauses = filtering.getNumAdmittedClauses();
    }
    
    std::vector<int> mergeClauseBuffersDuringAggregation(std::list<std::vector<int>>& elems) {

        // aggregate metadata
        int maxRevision = -1;
        int numAggregated = 0;
        int numInputLits = 0;
        int successfulSolverId = -1;
        long long bestFoundSolutionCost = LLONG_MAX;
        for (auto& elem : elems) {
            assert(elem.size() >= InplaceClauseAggregation::numMetadataInts()
                || log_return_false("[ERROR] Clause buffer has size %ld!\n", elem.size()));
            auto agg = InplaceClauseAggregation(elem);
            if (agg.successfulSolver() != -1 && (successfulSolverId == -1 || successfulSolverId > agg.successfulSolver())) {
                successfulSolverId = agg.successfulSolver();
            }
            numAggregated += agg.numAggregatedNodes();
            numInputLits += agg.numInputLiterals();
            maxRevision = std::max(maxRevision, agg.maxRevision());
            bestFoundSolutionCost = std::min(bestFoundSolutionCost, agg.bestFoundSolutionCost());
            agg.stripToRawBuffer();
        }
        int buflim = _job->getBufferLimit(numAggregated, false);
        numInputLits = std::min(numInputLits, buflim);

        // actual merging
        std::vector<int> merged;
        float time = Timer::elapsedSeconds();
        const int maxEffectiveClsLen = _params.strictClauseLengthLimit()+ClauseMetadata::numInts();
        const int maxFreeEffectiveClsLen = _params.freeClauseLengthLimit()+ClauseMetadata::numInts();
        initMergeClauseStore();
        if (_priority_based_buffer_merging /*initialized by initMergeClauseStore()!*/) {
            auto merger = BufferMerger(_merge_store.get(), buflim, maxEffectiveClsLen, maxFreeEffectiveClsLen, false);
            for (auto& elem : elems) {
                merger.add(_merge_store->getBufferReader(elem.data(), elem.size()));
            }
            merged = merger.mergePriorityBased(_params, _excess_clauses_from_merge, _rng);
        } else {
            auto merger = BufferMerger(buflim, maxEffectiveClsLen, maxFreeEffectiveClsLen, false);
            for (auto& elem : elems) {
                merger.add(_merge_store->getBufferReader(elem.data(), elem.size()));
            }
            merged = merger.mergePreservingExcessWithRandomTieBreaking(_excess_clauses_from_merge, _rng);
        }
        time = Timer::elapsedSeconds() - time;
    
        LOG(V4_VVER, "%s : merged %i contribs rev=%i (inp=%i, t=%.4fs) ~> len=%i\n",
            _job->getLabel(), numAggregated, maxRevision, numInputLits, time, merged.size());
        InplaceClauseAggregation::prepareRawBuffer(merged,
            maxRevision, numInputLits, numAggregated, successfulSolverId,
            bestFoundSolutionCost);
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

            for (size_t i = ClauseMetadata::enabled() ? 2 : 0; i < elem.size(); i++) {
                filter[i] |= elem[i]; // bitwise OR
            }
        }

        if (ClauseMetadata::enabled()) {
            ClauseMetadata::writeUnsignedLong(maxMinEpochId, filter.data());
        }

        return filter;
    }

    void initMergeClauseStore() {
        if (_merge_store) return;
        auto params = _job->getClauseStoreParams();
        _merge_store.reset(new StaticClauseStore<false>(params, false, 256, true, INT32_MAX));
        _priority_based_buffer_merging = params.priorityBasedBufferMerging();
    }
};

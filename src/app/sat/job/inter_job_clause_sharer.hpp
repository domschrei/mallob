
#pragma once

#include <climits>
#include <memory>
#include <vector>

#include "app/app_message_subscription.hpp"
#include "app/sat/data/clause_histogram.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/data/solver_statistics.hpp"
#include "app/sat/job/clause_sharing_actor.hpp"
#include "app/sat/job/inplace_sharing_aggregation.hpp"
#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/filter/exact_clause_filter.hpp"
#include "app/sat/sharing/filter/filter_vector_builder.hpp"
#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/sharing/filter/importing_solver.hpp"
#include "app/sat/sharing/filter/in_place_clause_filtering.hpp"
#include "app/sat/sharing/generic_export_manager.hpp"
#include "app/sat/sharing/simple_export_manager.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "app/sat/sharing/store/static_clause_store.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "comm/binary_tree_buffer_limit.hpp"
#include "comm/msgtags.h"
#include "comm/mympi.hpp"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"

class InterJobClauseSharer : public ClauseSharingActor {

private:
    const int _group_id;
    const ctx_id_t _context_id;
    std::string _label;
    const int _single_buf_lim;

    int _comm_size;
    int _comm_rank;

    std::unique_ptr<GenericClauseStore> _clause_store;
    std::unique_ptr<GenericClauseFilter> _clause_filter;
    std::unique_ptr<GenericExportManager> _export_manager;

    std::vector<std::shared_ptr<PortfolioSolverInterface>> _dummy_solvers_vec {{nullptr}};
    std::unique_ptr<SolverStatistics> _solver_stats;
    std::vector<SolverStatistics*> _stats_vec {nullptr};

    int _epoch {-1};
    bool _has_prepared_internal_shared_clauses {false};
    std::vector<int> _internal_shared_clauses;
    int _nb_internal_shared_lits;

    std::vector<int> _cross_shared_clauses;
    bool _has_filtered_shared_clauses {false};
    std::vector<int> _filter_vector;
    bool _has_clauses_to_broadcast_internally {false};
    std::vector<int> _clauses_to_broadcast_internally;

    size_t _last_num_cross_cls_to_import {0};
    size_t _last_num_admitted_cross_cls_to_import {0};

    int _nb_orig_variables {0};
    int _min_admissible_var {-1};
    int _max_admissible_var {INT32_MAX};

    long long _best_found_solution_cost {LLONG_MAX};

public:
    InterJobClauseSharer(const Parameters& params, int groupId, ctx_id_t contextId, const std::string& label) :
        ClauseSharingActor(params), _group_id(groupId), _context_id(contextId), _label(label),
        _single_buf_lim(std::max(100'000, (int)params.clauseBufferLimitParam())),
        _clause_store(new StaticClauseStore<false>(getClauseStoreParams(),
                    false, 256, true,
                    _single_buf_lim*params.numExportChunks())),
        _clause_filter(new ExactClauseFilter(*_clause_store, params.clauseFilterClearInterval(), params.strictClauseLengthLimit()+ClauseMetadata::numInts())),
        _export_manager(new SimpleExportManager(*_clause_store.get(), *_clause_filter.get(),
                    _dummy_solvers_vec, _stats_vec, params.strictClauseLengthLimit()+ClauseMetadata::numInts())),
        _solver_stats(new SolverStatistics()), _stats_vec({_solver_stats.get()}) {}

    void updateCommunicator(int commSize, int commRank) {
        _comm_size = commSize;
        _comm_rank = commRank;
    }

    void setOriginalNbVariables(int nbVars) {
        _nb_orig_variables = nbVars;
        LOG(V4_VVER, "XTCS %i orig vars\n", _nb_orig_variables);
    }
    void setAdmissibleVariableRange(int minVar, int maxVar) {
        _min_admissible_var = std::max(0, minVar);
        _max_admissible_var = maxVar < 0 ? INT_MAX : maxVar;
        LOG(V4_VVER, "XTCS limit var interval to [%i,%i]\n", _min_admissible_var, _max_admissible_var);
    }

    void addInternalSharedClauses(std::vector<int>& clauses) {
        BufferReader reader = _clause_store->getBufferReader(clauses.data(), clauses.size());
        size_t nbAdded = 0;
        size_t nbOriginal = 0; // # added clauses that only have original problem variables
        size_t nbBlocked = 0;
        while (true) {
            Mallob::Clause clause = reader.getNextIncomingClause();
            if (!clause.begin) break;

            // Block clauses featuring non-admissible variables
            for (int i = ClauseMetadata::numInts(); i < clause.size; i++) {
                const int v = std::abs(clause.begin[i]);
                if (v < _min_admissible_var || v > _max_admissible_var) {
                    clause.begin = nullptr;
                    nbBlocked++;
                    break;
                }
            }
            if (!clause.begin) continue;

            // Adjust LBD value according to incremental variable domain heuristic
            int clauseLbd = clause.lbd;
            if (_cs_params.incrementalVariableDomainHeuristic() >= 1) {
                clauseLbd = clause.size-ClauseMetadata::numInts() == 1 ? 1 : 2;
                bool original = true;
                for (int i = ClauseMetadata::numInts(); i < clause.size; i++) {
                    // Each literal beyond the original variable range incurs a penalty.
                    if (std::abs(clause.begin[i]) > _nb_orig_variables) {
                        clauseLbd++;
                        original = false;
                    }
                }
                // Clamp the "accumulated penalty" to the maximum valid LBD value.
                clauseLbd = std::min(clauseLbd, clause.size-ClauseMetadata::numInts());
                nbOriginal += original;
            }

            _export_manager->produce(clause.begin, clause.size, clauseLbd, 0, _epoch);
            nbAdded++;
        }
        if (_cs_params.incrementalVariableDomainHeuristic() >= 1)
            LOG(V4_VVER, "XTCS added %lu/%lu ITCS clauses (%lu original)\n", nbAdded, nbAdded+nbBlocked, nbOriginal);
        else
            LOG(V4_VVER, "XTCS added %lu/%lu ITCS clauses\n", nbAdded, nbAdded+nbBlocked);
    }

    void updateBestFoundSolutionCost(long long cost) override {
        _best_found_solution_cost = std::min(_best_found_solution_cost, cost);
    }

    void broadcastCrossSharedClauses(std::vector<int>& clauses, int nbLits) {
        InplaceClauseAggregation::prepareRawBuffer(clauses,
            getClausesRevision(), nbLits, 1, -1,
            _best_found_solution_cost);
        _clauses_to_broadcast_internally = std::move(clauses);
        _has_clauses_to_broadcast_internally = true;
    }

    bool hasClausesToBroadcastInternally() const {
        return _has_clauses_to_broadcast_internally;
    }
    std::vector<int>&& getClausesToBroadcastInternally() {
        return std::move(_clauses_to_broadcast_internally);
    }

    virtual int getActorJobId() const override {
        return _group_id;
    }
    virtual int getActorContextId() const override {
        return _context_id;
    }
    virtual int getClausesRevision() const override {
        return 0;
    }
    virtual const char* getLabel() override {
        return _label.c_str();
    }
    virtual int getNbSharingParticipants() const override {
        return _comm_size;
    }

    virtual Parameters getClauseStoreParams() const override {
        Parameters params = _cs_params;
        if (params.incrementalVariableDomainHeuristic() >= 1) {
            // Prioritize all clauses by heuristic first, length second.
            params.lbdPriorityInner.set(true);
            params.lbdPriorityOuter.set(true);
            // Enable priority-based buffer merging to really enforce the heuristic-first concept.
            params.priorityBasedBufferMerging.set(true);
        }
        return params;
    }

    virtual void prepareSharing() override {
        if (_has_prepared_internal_shared_clauses) return;
        const int size = getBufferLimit(1, true);
        int nbExportedClauses;
        _internal_shared_clauses = _clause_store->exportBuffer(size, nbExportedClauses, _nb_internal_shared_lits,
            GenericClauseStore::ANY, /*sortClauses=*/true);
        _has_prepared_internal_shared_clauses = true;
        _epoch++;
        _clause_filter->updateEpoch(_epoch);
    }
    virtual bool hasPreparedSharing() override {return _has_prepared_internal_shared_clauses;}
    virtual std::vector<int> getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) override {
        successfulSolverId = -1;
        numLits = _nb_internal_shared_lits;
        _has_prepared_internal_shared_clauses = false;
        LOG(V4_VVER, "%s XTCS contrib size %i\n", _label.c_str(), _internal_shared_clauses.size());
        return _internal_shared_clauses;
    }
    virtual void filterSharing(int epoch, std::vector<int>&& clauseBuf) override {
        _cross_shared_clauses = std::move(clauseBuf);
        InplaceClauseAggregation agg(_cross_shared_clauses);
        updateBestFoundSolutionCost(agg.bestFoundSolutionCost());
        agg.stripToRawBuffer();
        auto reader = _clause_store->getBufferReader(_cross_shared_clauses.data(), _cross_shared_clauses.size());
        _filter_vector = FilterVectorBuilder(0UL, _epoch).build(reader, [&](Mallob::Clause& clause) {
            return _clause_filter->admitSharing(clause, _epoch);
        }, [&](int len) {
            _clause_filter->acquireLock(len);
        }, [&](int len) {
            _clause_filter->releaseLock(len);
        });
        _has_filtered_shared_clauses = true;
    }
    virtual bool hasFilteredSharing(int epoch) override {return _has_filtered_shared_clauses;}
    virtual std::vector<int> getLocalFilter(int epoch) override {_has_filtered_shared_clauses = false; return std::move(_filter_vector);}
    virtual void applyFilter(int epoch, std::vector<int>&& filter) override {
        InPlaceClauseFiltering filtering(_cs_params, _cross_shared_clauses.data(), _cross_shared_clauses.size(), filter.data(), filter.size());
        int buflen = filtering.applyAndGetNewSize();
        _cross_shared_clauses.resize(buflen);
        _last_num_cross_cls_to_import += filtering.getNumClauses();
        _last_num_admitted_cross_cls_to_import += filtering.getNumAdmittedClauses();
        
        std::vector<ImportingSolver> importingSolvers {ImportingSolver(_comm_rank, 0, _solver_stats.get(), nullptr)};

        BufferReader reader = _clause_store->getBufferReader(_cross_shared_clauses.data(), _cross_shared_clauses.size());
        while (true) {
            Mallob::Clause clause = reader.getNextIncomingClause();
            if (!clause.begin) break;
            auto producers = _clause_filter->confirmSharingAndGetProducers(clause, _epoch);
            importingSolvers[0].appendCandidate(clause, producers);
        }

        reader = _clause_store->getBufferReader(_cross_shared_clauses.data(), _cross_shared_clauses.size());
        reader.setFilterBitset(importingSolvers[0].filter);
        BufferBuilder builder(-1, 255, false);
        ClauseHistogram hist(_cs_params.strictClauseLengthLimit()+ClauseMetadata::numInts());
        while (true) {
            Mallob::Clause clause = reader.getNextIncomingClause();
            if (!clause.begin) break;
            if (builder.append(clause)) hist.increment(clause.size);
        }

        LOG(V4_VVER, "%s XTCS digest %s\n", _label.c_str(), hist.getReport().c_str());
        std::vector<int> output = std::move(builder.extractBuffer());
        broadcastCrossSharedClauses(output, builder.getNumAddedLits());

        _clause_filter->collectGarbage(Logger::getMainInstance());
    }
    virtual void digestSharingWithoutFilter(int epoch, std::vector<int>&& clauses, bool stateless) override {
        abort(); // TODO
    }
    virtual void returnClauses(std::vector<int>&& clauses) override {
        BufferReader reader = _clause_store->getBufferReader(clauses.data(), clauses.size());
        _clause_store->addClauses(reader, nullptr);
    }
    virtual void digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>&& clauses) override {
        abort(); // TODO
    }

    virtual int getLastAdmittedNumLits() override {
        return _last_num_admitted_cross_cls_to_import;
    }
    long long getBestFoundObjectiveCost() override {
        return _best_found_solution_cost;
    }
    virtual void setClauseBufferRevision(int revision) override {}

    virtual size_t getBufferLimit(int numAggregatedNodes, bool selfOnly) override {
        if (selfOnly) return _single_buf_lim;
        return BinaryTreeBufferLimit::getLimit(numAggregatedNodes,
            _single_buf_lim, 2*_single_buf_lim,
            BinaryTreeBufferLimit::BufferQueryMode::LIMITED);
    }
};

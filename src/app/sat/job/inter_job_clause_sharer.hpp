
#pragma once

#include <memory>
#include <vector>

#include "app/sat/job/clause_sharing_actor.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/filter/filter_vector_builder.hpp"
#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/sharing/filter/in_place_clause_filtering.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "comm/binary_tree_buffer_limit.hpp"
#include "util/params.hpp"

class InterJobClauseSharer : public ClauseSharingActor {

private:
    const int _id;
    const int _context_id;
    std::string _label;

    struct Address {int rank; int contextId;};
    std::vector<Address> _comm;

    std::unique_ptr<GenericClauseStore> _clause_store;
    std::unique_ptr<GenericClauseFilter> _clause_filter;

    bool _has_prepared_internal_shared_clauses {false};
    std::vector<int> _internal_shared_clauses;
    int _nb_internal_shared_lits;
    int _internal_revision;

    std::vector<int> _cross_shared_clauses;
    bool _has_filtered_shared_clauses {false};
    std::vector<int> _filter_vector;

    size_t _last_num_cross_cls_to_import {0};
    size_t _last_num_admitted_cross_cls_to_import {0};

public:
    InterJobClauseSharer(const Parameters& params, int id, int contextId, const std::string& label) :
        ClauseSharingActor(params), _id(id), _context_id(contextId), _label(label) {}

    // TODO methods for (re-)computing communicator

    void addInternalSharedClauses(std::vector<int>& clauses) {
        BufferReader reader = _clause_store->getBufferReader(clauses.data(), clauses.size());
        _clause_store->addClauses(reader, nullptr);
        prepareSharing();
    }

    void broadcastCrossSharedClauses(std::vector<int>& clauses) {
        // TODO
    }




    virtual int getActorJobId() const override {
        return _id;
    }
    virtual int getActorContextId() const override {
        return _context_id;
    }
    virtual int getClausesRevision() const override {
        return _internal_revision;
    }
    virtual const char* getLabel() override {
        return _label.c_str();
    }
    virtual int getNbSharingParticipants() const override {
        return _comm.size();
    }

    virtual void prepareSharing() override {
        const int singleLimit = std::max(100'000, (int)_cs_params.clauseBufferLimitParam());
        const int size = BinaryTreeBufferLimit::getLimit(_comm.size(), singleLimit, 5*singleLimit, BinaryTreeBufferLimit::LIMITED);
        int nbExportedClauses;
        int nbExportedLits;
        _internal_shared_clauses = _clause_store->exportBuffer(size, nbExportedClauses, nbExportedLits,
            GenericClauseStore::ANY, /*sortClauses=*/true);
        _has_prepared_internal_shared_clauses = true;
    }
    virtual bool hasPreparedSharing() override {return _has_prepared_internal_shared_clauses;}
    virtual std::vector<int> getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) override {
        successfulSolverId = -1;
        numLits = _nb_internal_shared_lits;
        _has_prepared_internal_shared_clauses = false;
        return _internal_shared_clauses;
    }
    virtual void filterSharing(int epoch, std::vector<int>& clauseBuf) override {
        _cross_shared_clauses = std::move(clauseBuf);
        auto reader = _clause_store->getBufferReader(_cross_shared_clauses.data(), _cross_shared_clauses.size());
        _filter_vector = FilterVectorBuilder(0UL, epoch).build(reader, [&](Mallob::Clause& clause) {
            return _clause_filter->admitSharing(clause, epoch);
        }, [&](int len) {
            _clause_filter->acquireLock(len);
        }, [&](int len) {
            _clause_filter->releaseLock(len);
        });
        _has_filtered_shared_clauses = true;
    }
    virtual bool hasFilteredSharing(int epoch) override {return _has_filtered_shared_clauses;}
    virtual std::vector<int> getLocalFilter(int epoch) override {_has_filtered_shared_clauses = false; return std::move(_filter_vector);}
    virtual void applyFilter(int epoch, std::vector<int>& filter) override {
        InPlaceClauseFiltering filtering(_cs_params, _cross_shared_clauses.data(), _cross_shared_clauses.size(), filter.data(), filter.size());
        int buflen = filtering.applyAndGetNewSize();
        _cross_shared_clauses.resize(buflen);
        _last_num_cross_cls_to_import += filtering.getNumClauses();
        _last_num_admitted_cross_cls_to_import += filtering.getNumAdmittedClauses();
        broadcastCrossSharedClauses(_cross_shared_clauses);
    }
    virtual void digestSharingWithoutFilter(int epoch, std::vector<int>& clauses) override {
        broadcastCrossSharedClauses(_cross_shared_clauses);
    }
    virtual void returnClauses(std::vector<int>& clauses) override {
        BufferReader reader = _clause_store->getBufferReader(clauses.data(), clauses.size());
        _clause_store->addClauses(reader, nullptr);
    }
    virtual void digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>& clauses) override {
        broadcastCrossSharedClauses(_cross_shared_clauses); // TODO ?
    }

    virtual int getLastAdmittedNumLits() override {
        return _last_num_admitted_cross_cls_to_import;
    }
    virtual void setClauseBufferRevision(int revision) override {
        _internal_revision = revision;
    }
};

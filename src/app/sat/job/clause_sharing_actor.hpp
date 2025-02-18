
#pragma once

#include <vector>

#include "comm/binary_tree_buffer_limit.hpp"
#include "data/checksum.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"

class ClauseSharingActor {

protected:
    const Parameters& _cs_params;

    int _clsbuf_export_limit {0};

    float _compensation_factor = 1.0f;

    int _last_num_input_lits {0};
    float _estimate_incoming_lits = 0;
    float _accumulated_desired_lits = 0;
    float _accumulated_shared_lits = 0;
    float _estimate_shared_lits = -1.f;

    // stats
    size_t _total_desired {0};
    size_t _total_shared {0};

public:
    ClauseSharingActor(const Parameters& params) : _cs_params(params) {}

    virtual int getActorJobId() const = 0;
    virtual int getActorContextId() const = 0;
    virtual int getClausesRevision() const = 0;
    virtual const char* getLabel() = 0;
    virtual int getNbSharingParticipants() const = 0;

    virtual void prepareSharing() = 0;
    virtual bool hasPreparedSharing() = 0;
    virtual std::vector<int> getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) = 0;
    virtual void filterSharing(int epoch, std::vector<int>&& clauses) = 0;
    virtual bool hasFilteredSharing(int epoch) = 0;
    virtual std::vector<int> getLocalFilter(int epoch) = 0;
    virtual void applyFilter(int epoch, std::vector<int>&& filter) = 0;
    virtual void digestSharingWithoutFilter(int epoch, std::vector<int>&& clauses, bool stateless) = 0;
    virtual void returnClauses(std::vector<int>&& clauses) = 0;
    virtual void digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>&& clauses) = 0;

    virtual int getLastAdmittedNumLits() = 0;
    virtual long long getBestFoundObjectiveCost() = 0;
    virtual void setClauseBufferRevision(int revision) = 0;
    virtual void updateBestFoundSolutionCost(long long bestFoundSolutionCost) = 0;

    virtual Parameters getClauseStoreParams() const = 0;

    float updateSharingCompensationFactor() {

        int nbAdmittedLits = getLastAdmittedNumLits();
        auto defaultBuflim = BinaryTreeBufferLimit::getLimit(getNbSharingParticipants(),
            _cs_params.clauseBufferBaseSize(), _cs_params.clauseBufferLimitParam(),
            BinaryTreeBufferLimit::BufferQueryMode(_cs_params.clauseBufferLimitMode()));
        float priorCompensationFactor = _compensation_factor;
        int priorLimit = (int)std::ceil(priorCompensationFactor*defaultBuflim);

        if (_cs_params.compensateUnusedSharingVolume()) {

            if (_estimate_shared_lits <= 0) {
                // initialize estimate
                _estimate_shared_lits = nbAdmittedLits;
            } else {
                // update internal state
                _accumulated_shared_lits = 0.9f * _accumulated_shared_lits + nbAdmittedLits;
                _accumulated_desired_lits = std::max(1.f, 0.9f * _accumulated_desired_lits
                    // As target sharing volume, we want the base, default buffer limit
                    // but only as far as the number of input literals can actually fill it.
                    // To gauge this, we use the ratio between the last incoming lits and the last limit.
                    + std::min(defaultBuflim * _last_num_input_lits / priorLimit, defaultBuflim));
                _estimate_incoming_lits = 0.6 * _estimate_incoming_lits + 0.4 * (_last_num_input_lits / _compensation_factor);
                _estimate_shared_lits = 0.6 * _estimate_shared_lits + 0.4 * (nbAdmittedLits / _compensation_factor);
                // just for stats
                _total_desired += _last_num_input_lits;
                _total_shared += nbAdmittedLits;
            }
            LOG(V4_VVER, "CS estinc=%.1f estshr=%.1f accdes=%.1f accshr=%.1f totdes=%lu totshr=%lu\n",
                _estimate_incoming_lits, _estimate_shared_lits, _accumulated_desired_lits, _accumulated_shared_lits, _total_desired, _total_shared);

            _compensation_factor = _estimate_shared_lits <= 0 ? 1.0 :
                (_accumulated_desired_lits - _accumulated_shared_lits + _estimate_incoming_lits) / _estimate_shared_lits;
            _compensation_factor = 0.2 * priorCompensationFactor + 0.8 * _compensation_factor; // elastic update
        } else {
            _compensation_factor = 1;
        }

        _compensation_factor = std::max(1.0f / _cs_params.maxSharingCompensationFactor(),
            std::min(_cs_params.maxSharingCompensationFactor(), _compensation_factor));

        LOG(V3_VERB, "CS last sharing: %i/%i/%i globally passed ~> c=%.3f\n",
            nbAdmittedLits, _last_num_input_lits, priorLimit, _compensation_factor);

        return _compensation_factor;
    }
    int setSharingCompensationFactorAndUpdateExportLimit(float factor) {
        _compensation_factor = factor;
        _clsbuf_export_limit = getBufferLimit(1, true);
        return _clsbuf_export_limit;
    }
    void setNumInputLitsOfLastSharing(int numInputLits) {
        _last_num_input_lits = numInputLits;
        if (_estimate_incoming_lits <= 0) _estimate_incoming_lits = numInputLits;
    }

    virtual size_t getBufferLimit(int numAggregatedNodes, bool selfOnly) {
        if (selfOnly) return _compensation_factor * _cs_params.clauseBufferBaseSize();
        return _compensation_factor * BinaryTreeBufferLimit::getLimit(numAggregatedNodes,
            _cs_params.clauseBufferBaseSize(), _cs_params.clauseBufferLimitParam(),
            BinaryTreeBufferLimit::BufferQueryMode(_cs_params.clauseBufferLimitMode()));
    }
};


#pragma once

#include "app/app_message_subscription.hpp"
#include "app/job.hpp"
#include "data/checksum.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "util/logger.hpp"

class AnytimeSatClauseCommunicator; // fwd decl

class BaseSatJob : public Job {

public:
    BaseSatJob(const Parameters& params, const JobSetup& setup, AppMessageTable& appMsgTable) : 
        Job(params, setup, appMsgTable) {

        // Launched in certified UNSAT mode?
        if (params.proofOutputFile.isSet() || _params.onTheFlyChecking()) {
            
            // Check that the restrictions of this mode are met
            if (params.proofOutputFile.isSet() && !params.monoFilename.isSet()) {
                LOG(V0_CRIT, "[ERROR] Mallob was launched with certified UNSAT support "
                    "which only supports -mono mode of operation.\n");
                abort();
            }
            if (params.proofOutputFile.isSet() && !params.logDirectory.isSet()) {
                LOG(V0_CRIT, "[ERROR] Mallob was launched with proof writing "
                    "which requires providing a log directory.\n");
                abort();
            }
            if (params.proofOutputFile.isSet() && params.onTheFlyChecking()) {
                LOG(V0_CRIT, "[ERROR] Mallob does not yet support proof writing "
                    "and on-the-fly checking at the same time.\n");
                abort();
            }
            
            ClauseMetadata::enableClauseIds();
            if (_params.onTheFlyChecking()) {
                ClauseMetadata::enableClauseSignatures();
            }
        }
    }
    virtual ~BaseSatJob() {
        if (_estimate_shared_lits != -1.f) {
            LOG(V3_VERB, "%s CS total expected=%lu exchanged=%lu ratio=%.3f\n", toStr(), 
                _total_desired, _total_shared, _total_desired/(float)_total_shared);
        }
    }

    // Methods common to all BaseSatJob instances

    virtual bool isInitialized() = 0;
    
    virtual void prepareSharing() = 0;
    virtual bool hasPreparedSharing() = 0;
    virtual std::vector<int> getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) = 0;
    virtual int getLastAdmittedNumLits() = 0;
    virtual void setClauseBufferRevision(int revision) = 0;

    virtual void filterSharing(int epoch, std::vector<int>& clauses) = 0;
    virtual bool hasFilteredSharing(int epoch) = 0;
    virtual std::vector<int> getLocalFilter(int epoch) = 0;
    virtual void applyFilter(int epoch, std::vector<int>& filter) = 0;
    
    virtual void digestSharingWithoutFilter(std::vector<int>& clauses) = 0;
    virtual void returnClauses(std::vector<int>& clauses) = 0;
    virtual void digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>& clauses) = 0;

    // Methods common to all Job instances

    virtual void appl_start() override = 0;
    virtual void appl_suspend() override = 0;
    virtual void appl_resume() override = 0;
    virtual void appl_terminate() override = 0;

    virtual int appl_solved() override = 0;
    virtual JobResult&& appl_getResult() override = 0;
    
    virtual void appl_communicate() override = 0;
    virtual void appl_communicate(int source, int mpiTag, JobMessage& msg) override = 0;
    
    virtual void appl_dumpStats() override = 0;
    virtual bool appl_isDestructible() override = 0;
    virtual void appl_memoryPanic() override = 0;

    virtual bool checkResourceLimit(float wcSecsPerInstance, float cpuSecsPerInstance) override {
        if (!_done_solving && _params.satSolvingWallclockLimit() > 0) {
            auto age = getAgeSinceActivation();
            if (age > _params.satSolvingWallclockLimit()) {
                LOG(V2_INFO, "#%i SOLVING TIMEOUT: aborting\n", getId());
                return true;
            }
        }
        return Job::checkResourceLimit(wcSecsPerInstance, cpuSecsPerInstance);
    }
    void setSolvingDone() {
        _done_solving = true;
    }

protected:
    std::shared_ptr<AnytimeSatClauseCommunicator> _clause_comm;
    int _clsbuf_export_limit {0};

private:
    float _compensation_factor = 1.0f;

    int _last_num_input_lits {0};
    float _estimate_incoming_lits = 0;
    float _accumulated_desired_lits = 0;
    float _accumulated_shared_lits = 0;
    float _estimate_shared_lits = -1.f;

    // stats
    size_t _total_desired {0};
    size_t _total_shared {0};

    bool _done_solving = false;

    struct DeferredJobMsg {int source; int mpiTag; JobMessage msg;};
    std::list<DeferredJobMsg> _deferred_messages;

public:
    // Helper methods

    float updateSharingCompensationFactor() {

        int nbAdmittedLits = getLastAdmittedNumLits();
        auto defaultBuflim = MyMpi::getBinaryTreeBufferLimit(getVolume(),
            _params.clauseBufferBaseSize(), _params.clauseBufferLimitParam(),
            MyMpi::BufferQueryMode(_params.clauseBufferLimitMode()));
        float priorCompensationFactor = _compensation_factor;

        if (_params.compensateUnusedSharingVolume()) {

            if (_estimate_shared_lits == -1.f) {
                // initialize expected next sharing volume
                _estimate_shared_lits = _last_num_input_lits;
            } else {
                // update internal state
                _accumulated_shared_lits = 0.9f * _accumulated_shared_lits + nbAdmittedLits;
                _accumulated_desired_lits = std::max(1.f, 0.9f * _accumulated_desired_lits
                    // as target sharing volume, we want the base, default buffer limit
                    // but only as far as the number of input literals can actually fill it
                    + std::min(_last_num_input_lits, (int)defaultBuflim));
                _estimate_incoming_lits = 0.6 * _estimate_incoming_lits + 0.4 * (_last_num_input_lits / _compensation_factor);
                _estimate_shared_lits = 0.6 * _estimate_shared_lits + 0.4 * (nbAdmittedLits / _compensation_factor);
                // just for stats
                _total_desired += _last_num_input_lits;
                _total_shared += nbAdmittedLits;
            }
            LOG(V4_VVER, "%s CS estinc=%.1f estshr=%.1f accdes=%.1f accshr=%.1f totdes=%lu totshr=%lu\n",
                toStr(), _estimate_incoming_lits, _estimate_shared_lits, _accumulated_desired_lits, _accumulated_shared_lits, _total_desired, _total_shared);

            _compensation_factor = _estimate_shared_lits <= 0 ? 1.0 :
                (_accumulated_desired_lits - _accumulated_shared_lits + _estimate_incoming_lits) / _estimate_shared_lits;
        } else {
            _compensation_factor = 1;
        }

        _compensation_factor = std::max(0.1f, std::min((float)_params.maxSharingCompensationFactor(), _compensation_factor));

        LOG(V3_VERB, "%s CS last sharing: %i/%i/%i globally passed ~> c=%.3f\n", toStr(),
            nbAdmittedLits, _last_num_input_lits, (int)std::ceil(priorCompensationFactor*defaultBuflim),
            _compensation_factor);

        return _compensation_factor;
    }
    int setSharingCompensationFactorAndUpdateExportLimit(float factor) {
        _compensation_factor = factor;
        _clsbuf_export_limit = getBufferLimit(1, true);
        return _clsbuf_export_limit;
    }
    void setNumInputLitsOfLastSharing(int numInputLits) {
        _last_num_input_lits = numInputLits;
    }

    size_t getBufferLimit(int numAggregatedNodes, bool selfOnly) {
        if (selfOnly) return _compensation_factor * _params.clauseBufferBaseSize();
        return _compensation_factor * MyMpi::getBinaryTreeBufferLimit(numAggregatedNodes,
            _params.clauseBufferBaseSize(), _params.clauseBufferLimitParam(),
            MyMpi::BufferQueryMode(_params.clauseBufferLimitMode()));
    }

    void deferMessage(int source, int mpiTag, JobMessage& msg) {
        LOG(V3_VERB, "%s : deferring application msg\n", toStr());
        _deferred_messages.push_front(DeferredJobMsg {source, mpiTag, std::move(msg)});
    }

    bool hasDeferredMessage() const {return !_deferred_messages.empty();}

    DeferredJobMsg getDeferredMessage() {
        LOG(V3_VERB, "%s : fetching deferred application msg\n", toStr());
        auto result = std::move(_deferred_messages.back());
        _deferred_messages.pop_back();
        return result;
    }
};

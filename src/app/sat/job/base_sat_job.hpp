
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/job.hpp"
#include "data/checksum.hpp"
#include "app/sat/data/clause_metadata.hpp"

class AnytimeSatClauseCommunicator; // fwd decl

class BaseSatJob : public Job {

public:
    BaseSatJob(const Parameters& params, const JobSetup& setup, AppMessageTable& appMsgTable) : 
        Job(params, setup, appMsgTable) {

        // Launched in certified UNSAT mode?
        if (params.certifiedUnsat()) {
            
            // Check that the restrictions of this mode are met
            if (!params.monoFilename.isSet()) {
                LOG(V0_CRIT, "[ERROR] Mallob was launched with certified UNSAT support "
                    "which only supports -mono mode of operation.\n");
                abort();
            }
            if (!params.logDirectory.isSet()) {
                LOG(V0_CRIT, "[ERROR] Mallob was launched with certified UNSAT support "
                    "which requires providing a log directory.\n");
                abort();
            }
            
            ClauseMetadata::enableClauseIds();
        }
    }
    virtual ~BaseSatJob() {}

    // Methods common to all BaseSatJob instances

    virtual bool isInitialized() = 0;
    
    virtual void prepareSharing() = 0;
    virtual bool hasPreparedSharing() = 0;
    virtual std::vector<int> getPreparedClauses(Checksum& checksum, int& successfulSolverId) = 0;
    virtual std::pair<int, int> getLastAdmittedClauseShare() = 0;

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
    const float _compensation_decay {0.6};

    bool _done_solving = false;

    struct DeferredJobMsg {int source; int mpiTag; JobMessage msg;};
    std::list<DeferredJobMsg> _deferred_messages;

public:
    // Helper methods

    float updateSharingCompensationFactor() {

        auto [nbAdmitted, nbBroadcast] = getLastAdmittedClauseShare();
        float admittedRatio = nbBroadcast == 0 ? 1 : ((float)nbAdmitted) / nbBroadcast;
        admittedRatio = std::max(0.01f, admittedRatio);
        float newCompensationFactor = std::max(1.f, std::min(
            (float)_params.maxSharingCompensationFactor(), 1.f/admittedRatio
        ));
        _compensation_factor = _compensation_decay * _compensation_factor 
            + (1-_compensation_decay) * newCompensationFactor;

        LOG(V3_VERB, "%s CS last sharing: %i/%i globally passed ~> c=%.3f\n", toStr(), 
            nbAdmitted, nbBroadcast, _compensation_factor);

        return _compensation_factor;
    }
    void setSharingCompensationFactorAndUpdateExportLimit(float factor) {
        _compensation_factor = factor;
        _clsbuf_export_limit = getBufferLimit(1, MyMpi::SELF);
    }

    size_t getBufferLimit(int numAggregatedNodes, MyMpi::BufferQueryMode mode) {
        if (mode == MyMpi::SELF) return _compensation_factor * _params.clauseBufferBaseSize();
        return _compensation_factor * MyMpi::getBinaryTreeBufferLimit(numAggregatedNodes, 
            _params.clauseBufferBaseSize(), _params.clauseBufferDiscountFactor(), mode);
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

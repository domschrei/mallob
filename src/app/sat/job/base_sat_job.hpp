
#pragma once

#include "app/app_message_subscription.hpp"
#include "app/job.hpp"
#include "app/sat/job/clause_sharing_actor.hpp"
#include "data/checksum.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "util/logger.hpp"

class AnytimeSatClauseCommunicator; // fwd decl

class BaseSatJob : public Job, public ClauseSharingActor {

public:
    BaseSatJob(const Parameters& params, const JobSetup& setup, AppMessageTable& appMsgTable) : 
        Job(params, setup, appMsgTable), ClauseSharingActor(params) {

        // Launched in certified UNSAT mode?
        if (params.proofOutputFile.isSet() || _params.onTheFlyChecking()) {
            
            // Check that the restrictions of this mode are met
            if (params.proofOutputFile.isSet() && !params.monoFilename.isSet()) {
                LOG(V0_CRIT, "[ERROR] Mallob was launched with certified UNSAT support "
                    "which only supports -mono mode of operation.\n");
                abort();
            }
            if (params.proofOutputFile.isSet() && !params.proofDirectory.isSet()) {
                LOG(V0_CRIT, "[ERROR] Mallob was launched with proof writing "
                    "which requires providing a proof directory (-proof-dir).\n");
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

private:
    bool _done_solving = false;

    struct DeferredJobMsg {int source; int mpiTag; JobMessage msg;};
    std::list<DeferredJobMsg> _deferred_messages;

protected:
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

public:
    virtual int getActorJobId() const override {return getId();}
    virtual int getActorContextId() const override {return getContextId();}
    virtual int getClausesRevision() const override {return getRevision();}
    virtual const char* getLabel() override {return toStr();}
    virtual int getNbSharingParticipants() const override {return getVolume();}
    Parameters getClauseStoreParams() const override {return _params;}
};

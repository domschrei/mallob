
#pragma once

#include <future>
#include <memory>

#include "app/sat/data/clause.hpp"
#include "util/params.hpp"
#include "util/hashing.hpp"
#include "../sharing/buffer/adaptive_clause_database.hpp"
#include "data/job_transfer.hpp"
#include "app/job.hpp"
#include "base_sat_job.hpp"
#include "clause_history.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "clause_sharing_session.hpp"
#include "app/sat/proof/proof_producer.hpp"

class AnytimeSatClauseCommunicator {

private:
    const Parameters _params;
    BaseSatJob* _job = NULL;
    bool _suspended = false;

    const int _clause_buf_base_size;
    const float _clause_buf_discount_factor;
    const bool _use_cls_history;

    AdaptiveClauseDatabase _cdb;
    ClauseHistory _cls_history;

    std::unique_ptr<ClauseSharingSession> _current_session;
    std::list<std::unique_ptr<ClauseSharingSession>> _cancelled_sessions;

    int _current_epoch = 0;
    float _time_of_last_epoch_initiation = 0;

    float _solving_time = 0;

    bool _sent_cert_unsat_ready_msg;
    int _num_ready_msgs_from_children = 0;

    JobMessage _msg_unsat_found;
    std::list<JobMessage> _deferred_sharing_initiation_msgs;

    bool _initiated_proof_assembly = false;
    std::unique_ptr<ProofProducer> _proof_producer;

public:
    AnytimeSatClauseCommunicator(const Parameters& params, BaseSatJob* job) : _params(params), _job(job), 
        _clause_buf_base_size(_params.clauseBufferBaseSize()), 
        _clause_buf_discount_factor(_params.clauseBufferDiscountFactor()),
        _use_cls_history(params.collectClauseHistory()),
        _cdb([&]() {
            AdaptiveClauseDatabase::Setup setup;
            setup.maxClauseLength = _params.strictClauseLengthLimit();
            setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
            setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
            setup.numLiterals = 0;
            return setup;
        }()),
        _cls_history(_params, _job->getBufferLimit(_job->getJobTree().getCommSize(), MyMpi::ALL), *job, _cdb),
        _sent_cert_unsat_ready_msg(!ClauseMetadata::enabled()) {

        _time_of_last_epoch_initiation = Timer::elapsedSecondsCached();
    }

    void communicate();
    void handle(int source, int mpiTag, JobMessage& msg);

    void feedHistoryIntoSolver();

    bool isDestructible();
    int getCurrentEpoch() const {return _current_epoch;}

    bool isDoneAssemblingProof() const {return _proof_producer && _proof_producer->isDoneAssemblingProof();}

private:
    bool handleClauseHistoryMessage(int source, int mpiTag, JobMessage& msg);
    bool handleProofProductionMessage(int source, int mpiTag, JobMessage& msg);
    bool handleClauseSharingMessage(int source, int mpiTag, JobMessage& msg);

    void addToClauseHistory(std::vector<int>& clauses, int epoch);

    void initiateClauseSharing(JobMessage& msg);
    void tryActivateDeferredSharingInitiation();
    
    void checkCertifiedUnsatReadyMsg();
    void setupProofProducer(JobMessage& msg);
    bool tryInitiateSharing();
};

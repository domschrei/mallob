
#pragma once

#include <future>
#include <memory>

#include "app/sat/data/clause.hpp"
#include "util/params.hpp"
#include "util/hashing.hpp"
#include "data/job_transfer.hpp"
#include "app/job.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "clause_sharing_session.hpp"
#include "app/sat/proof/proof_producer.hpp"

class BaseSatJob; // fwd decl
class HistoricClauseStorage; // fwd decl

class AnytimeSatClauseCommunicator {

private:
    const Parameters _params;
    BaseSatJob* _job = NULL;
    bool _suspended = false;

    std::unique_ptr<HistoricClauseStorage> _cls_history;

    std::unique_ptr<ClauseSharingSession> _current_session;
    std::list<std::unique_ptr<ClauseSharingSession>> _cancelled_sessions;

    int _current_epoch = 0;
    float _time_of_last_epoch_initiation = 0;
    float _time_of_last_epoch_conclusion = 0;

    float _solving_time = 0;

    bool _sent_cert_unsat_ready_msg;
    int _num_ready_msgs_from_children = 0;

    JobMessage _msg_unsat_found;
    std::list<JobMessage> _deferred_sharing_initiation_msgs;

    bool _initiated_proof_assembly = false;
    std::unique_ptr<ProofProducer> _proof_producer;

    int _last_skipped_epochs_warning {0};

public:
    AnytimeSatClauseCommunicator(const Parameters& params, BaseSatJob* job);

    void communicate();
    void handle(int source, int mpiTag, JobMessage& msg);

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

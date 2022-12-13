
#pragma once

#include <future>

#include "app/sat/data/clause.hpp"
#include "util/params.hpp"
#include "util/hashing.hpp"
#include "../sharing/buffer/adaptive_clause_database.hpp"
#include "data/job_transfer.hpp"
#include "app/job.hpp"
#include "base_sat_job.hpp"
#include "clause_history.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "../proof/proof_assembler.hpp"
#include "../proof/merging/distributed_proof_merger.hpp"
#include "../proof/merging/proof_merge_connector.hpp"
#include "util/small_merger.hpp"
#include "clause_sharing_session.hpp"

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

    std::list<ClauseSharingSession> _sessions;

    int _current_epoch = 0;
    float _time_of_last_epoch_initiation = 0;

    float _solving_time = 0;
    float _reconstruction_time = 0;

    bool _sent_ready_msg = !ClauseMetadata::enabled();
    int _num_ready_msgs_from_children = 0;

    JobMessage _msg_unsat_found;
    std::list<JobMessage> _deferred_sharing_initiation_msgs;

    bool _initiated_proof_assembly = false;
    std::optional<ProofAssembler> _proof_assembler;
    std::optional<JobTreeAllReduction> _proof_all_reduction;
    bool _done_assembling_proof = false;
    std::vector<int> _proof_all_reduction_result;

    std::unique_ptr<DistributedProofMerger> _file_merger;
    std::vector<std::unique_ptr<MergeSourceInterface<SerializedLratLine>>> _local_merge_inputs;
    std::unique_ptr<SmallMerger<SerializedLratLine>> _local_merger;
    std::vector<ProofMergeConnector*> _merge_connectors;

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
        _cls_history(_params, _job->getBufferLimit(_job->getJobTree().getCommSize(), MyMpi::ALL), *job, _cdb) {

        _time_of_last_epoch_initiation = Timer::elapsedSecondsCached();
    }

    ~AnytimeSatClauseCommunicator() {
        _sessions.clear();
    }

    void communicate();
    void handle(int source, int mpiTag, JobMessage& msg);

    void feedHistoryIntoSolver();

    bool isDestructible();
    int getCurrentEpoch() const {return _current_epoch;}

    bool isDoneAssemblingProof() const {return _done_assembling_proof;}

private:

    std::vector<ProofMergeConnector*> setUpProofMerger(int threadsPerWorker);

    void addToClauseHistory(std::vector<int>& clauses, int epoch);

    void createNewProofAllReduction();
    void initiateClauseSharing(JobMessage& msg);
    void tryActivateDeferredSharingInitiation();
    
    bool tryInitiateSharing();
    inline ClauseSharingSession& currentSession() {return _sessions.back();}
};

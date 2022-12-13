
#include "anytime_sat_clause_communicator.hpp"

#include "util/logger.hpp"
#include "comm/mympi.hpp"
#include "../sharing/filter/clause_filter.hpp"
#include "util/sys/thread_pool.hpp"
#include "app/sat/proof/merging/proof_merge_file_input.hpp"
#include "clause_sharing_session.hpp"

void advanceCollective(BaseSatJob* job, JobMessage& msg, int broadcastTag) {
    if (job->getJobTree().isRoot() && msg.tag != broadcastTag) {
        // Self message: Switch from reduce to broadcast
        int oldTag = msg.tag;
        msg.tag = broadcastTag;
        MyMpi::isend(job->getJobTree().getRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        msg.tag = oldTag;
    } else if (msg.tag == broadcastTag) {
        // Broadcast to children
        if (job->getJobTree().hasLeftChild())
            MyMpi::isend(job->getJobTree().getLeftChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        if (job->getJobTree().hasRightChild())
            MyMpi::isend(job->getJobTree().getRightChildNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
    } else {
        // Reduce to parent
        MyMpi::isend(job->getJobTree().getParentNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
    }
}

void AnytimeSatClauseCommunicator::communicate() {

    if (_file_merger) {
        if (_file_merger->readyToMerge() && (!_params.interleaveProofMerging() || _proof_assembler->initialized())) {
            if (_params.interleaveProofMerging()) 
                _file_merger->setNumOriginalClauses(_proof_assembler->getNumOriginalClauses());
            _file_merger->beginMerge();
        } 
        
        if (_file_merger->beganMerging()) {
            _file_merger->advance();
            if (_file_merger->allProcessesFinished()) {
                // Proof merging done!
                if (!_done_assembling_proof && _job->getJobTree().isRoot()) {
                    _reconstruction_time = _job->getAgeSinceActivation() - _solving_time;
                    LOG(V2_INFO, "TIMING assembly %.3f\n", _reconstruction_time);
                }
                _done_assembling_proof = true;
            }
        }
    }

    // inactive?
    if (!_suspended && _job->getState() != ACTIVE) {
        // suspended!
        _suspended = true;
        if (_use_cls_history) _cls_history.onSuspend();
        // cancel any active sessions, sending neutral element upwards
        for (auto& session : _sessions) {
            session.cancel();
        }
        return;
    }
    if (_suspended) {
        if (_job->getState() == ACTIVE) _suspended = false;
        else return;
    }

    if (!_sent_ready_msg && _job->isInitialized()) {
        int numExpectedReadyMsgs = 0;
        if (2*_job->getIndex()+1 < _job->getGlobalNumWorkers()) 
            numExpectedReadyMsgs++;
        if (2*_job->getIndex()+2 < _job->getGlobalNumWorkers()) 
            numExpectedReadyMsgs++;

        if (numExpectedReadyMsgs == _num_ready_msgs_from_children) {
            _sent_ready_msg = true;
            if (!_job->getJobTree().isRoot()) {
                LOG(V3_VERB, "sending comm ready msg\n");
                JobMessage msg(_job->getId(), _job->getRevision(), 0, MSG_NOTIFY_READY_FOR_PROOF_SAFE_SHARING);
                MyMpi::isend(_job->getJobTree().getParentNodeRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
            } else {
                LOG(V3_VERB, "sharing enabled\n");
                if (!_proof_assembler.has_value() && !_msg_unsat_found.payload.empty()) {
                    // A solver has already found UNSAT which was deferred then.
                    // Now the message can be processed properly
                    LOG(V3_VERB, "Now processing deferred UNSAT notification\n");
                    MyMpi::isend(_job->getMyMpiRank(), MSG_SEND_APPLICATION_MESSAGE, _msg_unsat_found);
                }
            }
        }
    }

    // no communication at all?
    if (_params.appCommPeriod() <= 0) return;

    // clean up old sessions
    while (!_sessions.empty()) {
        auto& session = _sessions.front();
        if (!session.allStagesDone() || !session.isDestructible()) break;
        // can be deleted
        _sessions.pop_front();
    }

    // If a previous sharing initiation message has been deferred,
    // try to activate it now (if no sessions are active any more)
    tryActivateDeferredSharingInitiation();

    // Distributed proof assembly methods
    if (_proof_assembler.has_value()) {
        
        if ((!_proof_all_reduction.has_value() || !_proof_all_reduction->hasProducer()) 
                && _proof_assembler->canEmitClauseIds()) {
            // Export clause IDs via JobTreeAllReduction instance
            auto clauseIds = _proof_assembler->emitClauseIds();
            std::vector<int> clauseIdsIntVec((int*)clauseIds.data(), ((int*)clauseIds.data())+clauseIds.size()*2);
            _proof_all_reduction->produce([&]() {return clauseIdsIntVec;});
            LOG(V5_DEBG, "Emitted %i proof-relevant clause IDs\n", clauseIds.size());
        }

        if (_proof_all_reduction.has_value()) {
            _proof_all_reduction->advance();
            if (_proof_all_reduction->hasResult()) {
                _proof_all_reduction_result = _proof_all_reduction->extractResult();
                LOG(V5_DEBG, "Importing proof-relevant clause IDs\n");
                _proof_assembler->importClauseIds(
                    (LratClauseId*) _proof_all_reduction_result.data(), 
                    _proof_all_reduction_result.size()/2
                );
                _proof_all_reduction.reset();
                createNewProofAllReduction();
            }
        }

        if (_proof_assembler->finished()) {

            if (!_params.interleaveProofMerging()) {
                setUpProofMerger(-1);
                _file_merger->setNumOriginalClauses(_proof_assembler->getNumOriginalClauses());
            }

            _proof_all_reduction.reset();
            _proof_assembler.reset();
        }
    }

    // root: initiate sharing
    if (_job->getJobTree().isRoot() && tryInitiateSharing()) return;

    // any active sessions?
    if (_sessions.empty()) return;

    // Advance current sharing session
    auto& session = currentSession();
    if (session.isValid()) session.advanceSharing();
}

void AnytimeSatClauseCommunicator::handle(int source, int mpiTag, JobMessage& msg) {

    if (msg.jobId != _job->getId()) {
        LOG_ADD_SRC(V1_WARN, "[WARN] %s : stray job message meant for #%i\n", source, _job->toStr(), msg.jobId);
        return;
    }

    if (msg.tag == MSG_NOTIFY_READY_FOR_PROOF_SAFE_SHARING) {
        _num_ready_msgs_from_children++;
        LOG(V3_VERB, "got comm ready msg (total:%i)\n", _num_ready_msgs_from_children);
        return;
    }

    if (_job->getState() != ACTIVE) {
        // Not active any more: return message to sender
        if (!msg.returnedToSender) {
            msg.returnedToSender = true;
            MyMpi::isend(source, mpiTag, msg);
        }
        return;
    }

    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SEND_CLAUSES) {
        _cls_history.addEpoch(msg.epoch, msg.payload, /*entireIndex=*/true);
        _cls_history.sendNextBatches();
    }
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_SUBSCRIBE)
        _cls_history.onSubscribe(source, msg.payload[0], msg.payload[1]);
    if (msg.tag == ClauseHistory::MSG_CLAUSE_HISTORY_UNSUBSCRIBE)
        _cls_history.onUnsubscribe(source);

    // Process unsuccessful, returned messages
    if (msg.returnedToSender) {
        msg.returnedToSender = false;
        if (msg.tag == MSG_INITIATE_CLAUSE_SHARING) {
            // Initiation signal hit an inactive (?) child:
            // Pretend that it sent an empty set of clauses
            msg.tag = MSG_ALLREDUCE_CLAUSES;
            mpiTag = MSG_JOB_TREE_REDUCTION;
            msg.payload.resize(1);
            msg.payload[0] = 1; // num aggregated nodes
        } else if (msg.tag == MSG_ALLREDUCE_CLAUSES && mpiTag == MSG_JOB_TREE_BROADCAST) {
            // Distribution of clauses hit an inactive (?) child:
            // Pretend that it sent an empty filter
            msg.tag = MSG_ALLREDUCE_FILTER;
            mpiTag = MSG_JOB_TREE_REDUCTION;
            msg.payload.resize(ClauseMetadata::numBytes());
            for (int& num : msg.payload) num = 0;
        } else return;
    }

    bool forwardedInitiateProofMessage = false;
    if (msg.tag == MSG_NOTIFY_UNSAT_FOUND) {
        assert(_job->getJobTree().isRoot());
        
        if (!_sent_ready_msg) {
            // Job is not ready yet to reconstruct proofs.
            LOG(V2_INFO, "Deferring UNSAT notification since job is not yet ready\n");
            _msg_unsat_found = std::move(msg);
            return;
        }

        if (_proof_assembler.has_value()) {
            // Obsolete message
            LOG(V2_INFO, "Obsolete UNSAT notification - already assembling a proof\n");
            return;
        }

        // Initiate proof assembly
        msg.tag = MSG_INITIATE_PROOF_COMBINATION;
        msg.payload.push_back(_job->getGlobalNumWorkers());
        // Use *original* #threads, not adjusted #threads,
        // since proof instance IDs are assigned that way!
        msg.payload.push_back(_params.numThreadsPerProcess());
        //msg.payload.push_back(_job->getNumThreads());
        // vvv Advances in the next branch vvv
        forwardedInitiateProofMessage = true;
    }
    if (msg.tag == MSG_INITIATE_PROOF_COMBINATION) {

        // Guarantee that the proof combination is not initiated more than once
        if (_initiated_proof_assembly) return;
        _initiated_proof_assembly = true;
        _job->setSolvingDone();

        if (_job->getJobTree().isRoot()) {
            _solving_time = _job->getAgeSinceActivation();
            LOG(V2_INFO, "TIMING solving %.3f\n", _solving_time);
        }
        LOG(V2_INFO, "Initiate proof assembly\n");

        // Propagate initialization message
        advanceCollective(_job, msg, MSG_INITIATE_PROOF_COMBINATION);  
        if (forwardedInitiateProofMessage) {
            // send the initiation message explicitly again
            // to ensure that the job is terminated properly
            MyMpi::isend(_job->getMyMpiRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        }

        // Create _proof_assembler
        if (!_proof_assembler.has_value()) {
            int finalEpoch = msg.epoch;
            int winningInstance = msg.payload[0];
            unsigned long globalStartOfSuccessEpoch;
            memcpy(&globalStartOfSuccessEpoch, msg.payload.data()+1, 2*sizeof(int));
            int numWorkers = msg.payload[3];
            int threadsPerWorker = msg.payload[4];
            int thisWorkerIndex = _job->getJobTree().getIndex();
            _proof_assembler.emplace(_params, _job->getId(), numWorkers, threadsPerWorker, thisWorkerIndex, 
                finalEpoch, winningInstance, globalStartOfSuccessEpoch);
            createNewProofAllReduction();

            if (_params.interleaveProofMerging()) {
                // # local instances is the ACTUAL # threads, not the original one.
                _merge_connectors = setUpProofMerger(_job->getNumThreads());
                _proof_assembler->startWithInterleavedMerging(&_merge_connectors);
            } else {
                _proof_assembler->start();
            }
        }
    }
    if (msg.tag == MSG_ALLREDUCE_PROOF_RELEVANT_CLAUSES) {
        LOG(V5_DEBG, "Receiving %i proof-relevant clause IDs from epoch %i\n", msg.payload.size()/2, msg.epoch);
        assert(_proof_all_reduction.has_value());
        _proof_all_reduction->receive(source, mpiTag, msg);
        _proof_all_reduction->advance();
    }

    // Initial signal to initiate a sharing epoch
    if (msg.tag == MSG_INITIATE_CLAUSE_SHARING) {
        initiateClauseSharing(msg);
        return;
    }

    // Advance all-reductions
    bool success = false;
    if (!_sessions.empty()) {
        success = currentSession().advanceClauseAggregation(source, mpiTag, msg)
                || currentSession().advanceFilterAggregation(source, mpiTag, msg);
    }
    if (!success) {
        // Special case where clauses are broadcast but message was not processed:
        // Return an empty filter to the sender such that the sharing epoch may continue
        if (msg.tag == MSG_ALLREDUCE_CLAUSES && mpiTag == MSG_JOB_TREE_BROADCAST) {
            msg.payload.resize(ClauseMetadata::numBytes());
            for (int& num : msg.payload) num = 0;
            msg.tag = MSG_ALLREDUCE_FILTER;
            MyMpi::isend(source, MSG_JOB_TREE_REDUCTION, msg);
        }
    }
}

void AnytimeSatClauseCommunicator::initiateClauseSharing(JobMessage& msg) {
    if (!_sessions.empty() || !_deferred_sharing_initiation_msgs.empty()) {
        // defer message until all past sessions are done
        // and all earlier deferred initiation messages have been processed
        LOG(V3_VERB, "%s : deferring CS initiation\n", _job->toStr());
        _deferred_sharing_initiation_msgs.push_back(std::move(msg));
        return;
    }

    // no current sessions active - can start new session
    _current_epoch = msg.epoch;
    LOG(V5_DEBG, "%s : INIT COMM e=%i nc=%i\n", _job->toStr(), _current_epoch, 
        _job->getJobTree().getNumChildren());
    _sessions.emplace_back(_params, _job, _cdb, _cls_history, _current_epoch);
    if (!_job->hasPreparedSharing()) {
        int limit = _job->getBufferLimit(1, MyMpi::SELF);
        _job->prepareSharing(limit);
    }
    advanceCollective(_job, msg, MSG_INITIATE_CLAUSE_SHARING);
}

void AnytimeSatClauseCommunicator::tryActivateDeferredSharingInitiation() {
    
    // Anything to activate?
    if (_deferred_sharing_initiation_msgs.empty()) return;

    // cannot start new sharing if a session is still present
    if (!_sessions.empty()) return;

    // sessions are empty -> WILL succeed to initiate sharing
    // -> initiation message CAN be deleted afterwards.
    JobMessage msg = std::move(_deferred_sharing_initiation_msgs.front());
    _deferred_sharing_initiation_msgs.pop_front();
    initiateClauseSharing(msg);
}

void AnytimeSatClauseCommunicator::createNewProofAllReduction() {
    assert(!_proof_all_reduction.has_value());
    JobMessage baseMsg(_job->getId(), _job->getRevision(), _proof_assembler->getEpoch(), MSG_ALLREDUCE_PROOF_RELEVANT_CLAUSES);
    _proof_all_reduction.emplace(_job->getJobTree(), baseMsg, std::vector<int>(), [&](auto& list) {
        
        std::list<std::pair<LratClauseId*, size_t>> idArrays;
        for (auto& vec : list) {
            idArrays.emplace_back((LratClauseId*) vec.data(), vec.size() / 2);
        }

        auto longVecResult = _proof_assembler->mergeClauseIdVectors(idArrays);
        int* intResultData = (int*) longVecResult.data();

        std::vector<int> result(intResultData, intResultData + longVecResult.size()*2);
        return result;
    });
}

void AnytimeSatClauseCommunicator::feedHistoryIntoSolver() {
    if (_use_cls_history) _cls_history.feedHistoryIntoSolver();
}

bool AnytimeSatClauseCommunicator::tryInitiateSharing() {

    if (_proof_assembler.has_value() || _file_merger || !_sent_ready_msg) return false;

    auto time = Timer::elapsedSecondsCached();
    bool nextEpochDue = time - _time_of_last_epoch_initiation >= _params.appCommPeriod();
    bool lastEpochDone = _sessions.empty() || currentSession().hasConcludedFiltering();
    if (nextEpochDue && !lastEpochDone) {
        LOG(V1_WARN, "[WARN] %s : Next epoch over-due!\n", _job->toStr());
    }
    if (nextEpochDue && lastEpochDone) {
        _current_epoch++;
        JobMessage msg(_job->getId(), _job->getRevision(), _current_epoch, MSG_INITIATE_CLAUSE_SHARING);

        // Advance initiation time exactly by the specified period 
        // in order to lose no time for the subsequent epoch
        _time_of_last_epoch_initiation += _params.appCommPeriod();
        // If an epoch has already been skipped, just set the initiation to the current time
        if (time - _time_of_last_epoch_initiation >= _params.appCommPeriod())
            _time_of_last_epoch_initiation = time;
        
        // Self message to initiate clause sharing
        MyMpi::isend(_job->getJobTree().getRank(), MSG_SEND_APPLICATION_MESSAGE, msg);
        LOG(V4_VVER, "%s CS init\n", _job->toStr());
        return true;
    }
    return false;
}

bool AnytimeSatClauseCommunicator::isDestructible() {
    for (auto& session : _sessions) if (!session.isDestructible()) return false;
    return true;
}

std::vector<ProofMergeConnector*> AnytimeSatClauseCommunicator::setUpProofMerger(int numLocalInstances) {
    
    std::vector<ProofMergeConnector*> connectors;

    if (_params.interleaveProofMerging()) {

        // Populate _local_merge_inputs with connectors
        assert(numLocalInstances > 0);
        for (size_t i = 0; i < numLocalInstances; i++) {
            // Each of these will be connected to the output of a ProofInstance
            connectors.push_back(new SPSCBlockingRingbuffer<SerializedLratLine>(32768));
            _local_merge_inputs.emplace_back(connectors.back());
        }
        
    } else {

        // Populate _local_merge_inputs with local file inputs
        auto proofFiles = _proof_assembler->getProofOutputFiles();
        for (auto& proofFile : proofFiles) {
            _local_merge_inputs.emplace_back(new ProofMergeFileInput(proofFile));
        }
    }

    // Set up local merger: Merges together all local proof parts
    std::vector<MergeSourceInterface<SerializedLratLine>*> ptrs;
    for (auto& source : _local_merge_inputs) ptrs.push_back(source.get());
    _local_merger.reset(new SmallMerger<SerializedLratLine>(ptrs));

    // Set up distributed merge procedure
    _file_merger.reset(new DistributedProofMerger(MPI_COMM_WORLD, /*branchingFactor=*/6, 
        _local_merger.get(), _params.proofOutputFile()));

    // Register callback for processing merge messages
    MyMpi::getMessageQueue().registerCallback(MSG_ADVANCE_DISTRIBUTED_FILE_MERGE, [&](MessageHandle& h) {
        MergeMessage msg; msg.deserialize(h.getRecvData());
        _file_merger->handle(h.source, msg);
    });

    return connectors;
}

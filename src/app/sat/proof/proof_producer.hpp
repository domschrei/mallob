
#pragma once

#include "proof_assembler.hpp"
#include "merging/distributed_proof_merger.hpp"
#include "merging/proof_merge_connector.hpp"
#include "util/small_merger.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "app/sat/proof/merging/proof_merge_file_input.hpp"
#include "comm/msg_queue/message_subscription.hpp"

class ProofProducer {

public:
    struct ProofSetup {
        int jobId;
        int revision;
        int finalEpoch;
        int numWorkers;
        int threadsPerWorker;
        int thisJobNumThreads;
        int thisWorkerIndex;
        int winningInstance;
        unsigned long globalStartOfSuccessEpoch;
        float solvingTime;
        float jobAgeSinceActivation;
    };

private:
    const Parameters& _params;
    ProofSetup _setup;
    JobTree& _job_tree;

    std::unique_ptr<ProofAssembler> _proof_assembler;
    std::optional<JobTreeAllReduction> _proof_all_reduction;
    bool _done_assembling_proof = false;
    std::vector<int> _proof_all_reduction_result;

    std::unique_ptr<DistributedProofMerger> _file_merger;
    std::vector<std::unique_ptr<MergeSourceInterface<SerializedLratLine>>> _local_merge_inputs;
    std::unique_ptr<SmallMerger<SerializedLratLine>> _local_merger;
    std::vector<ProofMergeConnector*> _merge_connectors;

    float _reconstruction_time = 0;

    std::optional<MessageSubscription> _subscription_merge;

public:
    ProofProducer(const Parameters& params, const ProofSetup& setup, JobTree& jobTree) : 
        _params(params), _setup(setup), _job_tree(jobTree),
        _proof_assembler(new ProofAssembler(_params, setup.jobId, setup.numWorkers, setup.threadsPerWorker, setup.thisWorkerIndex, 
                setup.finalEpoch, setup.winningInstance, setup.globalStartOfSuccessEpoch)) {
        
        createNewProofAllReduction();

        if (_params.interleaveProofMerging()) {
            // # local instances is the ACTUAL # threads, not the original one.
            _merge_connectors = setUpProofMerger(_setup.thisJobNumThreads);
            _proof_assembler->startWithInterleavedMerging(&_merge_connectors);
        } else {
            _proof_assembler->start();
        }
    }

    void advanceFileMerger() {
        if (!_file_merger) return;

        assert(_proof_assembler);
        if (_file_merger->readyToMerge() && (!_params.interleaveProofMerging() || _proof_assembler->initialized())) {
            if (_params.interleaveProofMerging()) 
                _file_merger->setNumOriginalClauses(_proof_assembler->getNumOriginalClauses());
            _file_merger->beginMerge();
        } 
        
        if (_file_merger->beganMerging()) {
            _file_merger->advance();
            if (_file_merger->allProcessesFinished()) {
                // Proof merging done!
                if (!_done_assembling_proof && _job_tree.isRoot()) {
                    _reconstruction_time = _setup.jobAgeSinceActivation - _setup.solvingTime;
                    LOG(V2_INFO, "TIMING assembly %.3f\n", _reconstruction_time);
                }
                _done_assembling_proof = true;
            }
        }
    }

    void advanceProofAssembly() {
        if (!_proof_assembler) return;

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

        if (_proof_assembler->finished() && _proof_all_reduction) {
            if (!_params.interleaveProofMerging()) {
                setUpProofMerger(-1);
                _file_merger->setNumOriginalClauses(_proof_assembler->getNumOriginalClauses());
            }
            _proof_all_reduction.reset();
        }
    }

    void handle(int source, int mpiTag, JobMessage& msg) {
        LOG(V5_DEBG, "Receiving %i proof-relevant clause IDs from epoch %i\n", msg.payload.size()/2, msg.epoch);
        assert(_proof_all_reduction.has_value());
        _proof_all_reduction->receive(source, mpiTag, msg);
        _proof_all_reduction->advance();
    }

    bool isDoneAssemblingProof() const {
        return _done_assembling_proof;
    }

private:

    std::vector<ProofMergeConnector*> setUpProofMerger(int numLocalInstances) {
    
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
        _file_merger.reset(new DistributedProofMerger(_params, MPI_COMM_WORLD, /*branchingFactor=*/6, 
            _local_merger.get(), _params.proofOutputFile()));

        // Register callback for processing merge messages
        _subscription_merge = MessageSubscription(MSG_ADVANCE_DISTRIBUTED_FILE_MERGE, [&](MessageHandle& h) {
            MergeMessage msg; msg.deserialize(h.getRecvData());
            _file_merger->handle(h.source, msg);
        });

        return connectors;
    }

    void createNewProofAllReduction() {
        assert(!_proof_all_reduction.has_value());
        JobMessage baseMsg(_setup.jobId, 0, _setup.revision, 
            _proof_assembler->getEpoch(), MSG_ALLREDUCE_PROOF_RELEVANT_CLAUSES);

        _proof_all_reduction.emplace(_job_tree, baseMsg, std::vector<int>(), [&](auto& list) {
            
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

};

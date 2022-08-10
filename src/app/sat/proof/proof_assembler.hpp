
#include "proof_instance.hpp"

class ProofAssembler {

private:
    const Parameters& _params;
    int _job_id;
    int _num_workers;
    int _threads_per_worker;
    int _this_worker_index;
    int _final_epoch;
    int _winning_instance;
    std::list<ProofInstance> _proof_instances;

    int _current_epoch;

public:
    ProofAssembler(const Parameters& params, int jobId, int numWorkers, int threadsPerWorker, 
        int thisWorkerIndex, int finalEpoch, int winningInstance) :
            _params(params), _job_id(jobId), _num_workers(numWorkers), _threads_per_worker(threadsPerWorker), 
            _this_worker_index(thisWorkerIndex), _final_epoch(finalEpoch), _winning_instance(winningInstance) {

        createInstancesViaClauseEpochs(params.logDirectory() + "/proof#" + std::to_string(jobId) 
            + "/clauseepochs." + std::to_string(thisWorkerIndex));
        _current_epoch = _final_epoch;
        beginProofAssembly();
    }

    int getEpoch() const {
        return _current_epoch;
    }

    bool finished() const {
        for (const auto& inst : _proof_instances) {
            if (!inst.finished()) return false;
        }
        return true;
    }

    bool canEmitClauseIds() const {
        bool someReady = false;
        for (const auto& inst : _proof_instances) {
            if (!inst.finished()) {
                if (inst.ready()) someReady = true;
                else return false;
            }
        }
        return someReady;
    }

    std::vector<LratClauseId> emitClauseIds() {
        std::list<std::vector<LratClauseId>> instancesClauseLists;
        std::list<std::pair<LratClauseId*, size_t>> instancesIdArrays;
        for (auto& inst : _proof_instances) {
            instancesClauseLists.push_back(inst.extractNextOutgoingClauseIds());
            instancesIdArrays.emplace_back(instancesClauseLists.back().data(), instancesClauseLists.back().size());
        }
        return mergeClauseIdVectors(instancesIdArrays);
    }

    void importClauseIds(const LratClauseId* clauseIdsData, size_t clauseIdsSize) {
        _current_epoch--;
        for (auto& inst : _proof_instances) {
            inst.advance(clauseIdsData, clauseIdsSize);
        }
    }

    std::vector<LratClauseId> mergeClauseIdVectors(const std::list<std::pair<LratClauseId*, size_t>>& idArrays) {
        
        std::vector<LratClauseId> output;
        
        std::vector<LratClauseId*> pointers;
        std::vector<size_t> numRemaining;
        for (auto& [pointer, size] : idArrays) {
            pointers.push_back(pointer);
            numRemaining.push_back(size);
        }

        while (true) {
            bool anyLeft = false;
            size_t nextVecPos;
            LratClauseId nextClauseId;

            for (size_t i = 0; i < pointers.size(); i++) {
                auto& pointer = pointers[i];
                auto& remaining = numRemaining[i];
                if (remaining == 0) continue;
                auto clauseId = *pointer;
                if (!anyLeft || clauseId < nextClauseId) {
                    anyLeft = true;
                    nextVecPos = i;
                    nextClauseId = clauseId;
                }
            }
            if (!anyLeft) break;

            output.push_back(nextClauseId);
            numRemaining[nextVecPos]--;
        }

        return output;
    }

    std::vector<std::string> getProofOutputFiles() const {
        std::vector<std::string> outputs;
        for (auto& inst : _proof_instances) {
            outputs.push_back(inst.getOutputFilename());
        }
        return outputs;
    }

private:
    void createInstancesViaClauseEpochs(const std::string& filename) {

        std::vector<LratClauseId> globalIdStarts;
        std::vector<std::vector<LratClauseId>> localIdStartsPerInstance;
        std::vector<std::vector<LratClauseId>> localIdOffsetsPerInstance;

        std::ifstream ifs(filename);
        assert(ifs.is_open());
        std::string line;
        while (getline(ifs, line)) {
            
            // Split line by whitespaces
            std::istringstream buffer(line);
            std::vector<std::string> words;
            std::copy(std::istream_iterator<std::string>(buffer), 
                    std::istream_iterator<std::string>(),
                    std::back_inserter(words));

            // Epoch GlobalEpochId (MinEpochId Offset)*
            
            int epoch = atoi(words[0].c_str());
            int globalEpochId = atoi(words[1].c_str());
            globalIdStarts.push_back(globalEpochId);
            assert(epoch+1 == globalIdStarts.size());

            size_t i = 0;
            while (2 + 2*i + 1 < words.size()) {
                while (i >= localIdStartsPerInstance.size()) {
                    localIdStartsPerInstance.emplace_back();
                    localIdOffsetsPerInstance.emplace_back();
                }
                localIdStartsPerInstance.at(i).push_back(atoi(words[2+2*i].c_str()));
                localIdOffsetsPerInstance.at(i).push_back(atoi(words[2+2*i+1].c_str()));
            }
        }

        // Create proof instances
        for (size_t i = 0; i < localIdStartsPerInstance.size(); i++) {
            int instanceId = _this_worker_index * _threads_per_worker + i;
            int numInstances = _num_workers * _threads_per_worker;
            std::string proofFilenameBase = _params.logDirectory() + "/proof#" 
                + std::to_string(_job_id) + "/proof." + std::to_string(instanceId);

            _proof_instances.emplace_back(
                instanceId, numInstances, proofFilenameBase + ".lrat", 
                _final_epoch, _winning_instance, globalIdStarts,
                std::move(localIdStartsPerInstance[i]), std::move(localIdOffsetsPerInstance[i]), 
                proofFilenameBase + ".filtered.lrat"
            );
        }
    }

    void beginProofAssembly() {
        for (auto& inst : _proof_instances) {
            inst.advance(nullptr, 0);
        }
    }
};

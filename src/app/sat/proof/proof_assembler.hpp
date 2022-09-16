
#include "proof_instance.hpp"
#include "util/logger.hpp"

#include <stdlib.h>

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

    unsigned long _num_original_clauses;
    int _current_epoch;

    bool _initialized = false;

    std::future<void> _fut_begin_assembly;

public:
    ProofAssembler(const Parameters& params, int jobId, int numWorkers, int threadsPerWorker, 
        int thisWorkerIndex, int finalEpoch, int winningInstance) :
            _params(params), _job_id(jobId), _num_workers(numWorkers), _threads_per_worker(threadsPerWorker), 
            _this_worker_index(thisWorkerIndex), _final_epoch(finalEpoch), _winning_instance(winningInstance) {

        _current_epoch = _final_epoch;
    }

    void start() {
        startWithInterleavedMerging(nullptr);
    }

    void startWithInterleavedMerging(std::vector<ProofMergeConnector>* connectors) {
        _fut_begin_assembly = ProcessWideThreadPool::get().addTask([&, connectors]() {
            createInstancesViaClauseEpochs(_params.logDirectory() + "/proof#" + std::to_string(_job_id) 
                + "/clauseepochs." + std::to_string(_this_worker_index));
            if (connectors) assert(connectors->size() == _proof_instances.size() 
                || log_return_false("%i != %i\n", connectors->size(), _proof_instances.size()));
            beginProofAssembly(connectors);
        });
    }

    bool initialized() const {return _initialized;}

    ~ProofAssembler() {
        if (_fut_begin_assembly.valid()) _fut_begin_assembly.get();
    }

    int getEpoch() const {
        return _current_epoch;
    }

    bool finished() const {
        if (!_initialized) return false;
        for (const auto& inst : _proof_instances) {
            if (!inst.finished()) return false;
        }
        return true;
    }

    bool canEmitClauseIds() const {
        if (!_initialized) return false;
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
        if (_this_worker_index == 0) {
            //std::string clsStr;
            //for (size_t i = 0; i < clauseIdsSize; i++) clsStr += std::to_string(clauseIdsData[i]) + " ";
            LOG(V2_INFO, "Proof e.%i: %i shared IDs\n", _current_epoch, clauseIdsSize);
        }
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
                // Skip all duplicate IDs
                while (!output.empty() && remaining>0 && *pointer == output.back()) {
                    remaining--;
                    pointer++;
                }
                if (remaining == 0) continue;
                // LARGEST ID first
                auto clauseId = *pointer;
                if (!anyLeft || clauseId > nextClauseId) {
                    anyLeft = true;
                    nextVecPos = i;
                    nextClauseId = clauseId;
                }
            }
            if (!anyLeft) break;

            output.push_back(nextClauseId);
            numRemaining[nextVecPos]--;
            pointers[nextVecPos]++;
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

    unsigned long getNumOriginalClauses() const {
        return _num_original_clauses;
    }

private:
    void createInstancesViaClauseEpochs(const std::string& filename) {

        std::vector<LratClauseId> globalIdStarts;
        std::vector<std::vector<LratClauseId>> localIdStartsPerInstance;
        std::vector<std::vector<LratClauseId>> localIdOffsetsPerInstance;

        LOG(V2_INFO, "PrAs Waiting for clause epochs file \"%s\" ...\n", filename.c_str());
        std::ifstream ifs(filename);
        while (!ifs.is_open()) {
            usleep(1000 * 100);
            ifs.open(filename);
        }

        std::string line;
        auto& success = getline(ifs, line);
        assert(success);
        _num_original_clauses = std::stoul(line);

        LOG(V2_INFO, "PrAs clause epochs file opened, %lu original clauses\n", _num_original_clauses);

        while (getline(ifs, line)) {
            
            // Split line by whitespaces
            std::istringstream buffer(line);
            std::vector<std::string> words;
            std::copy(std::istream_iterator<std::string>(buffer), 
                    std::istream_iterator<std::string>(),
                    std::back_inserter(words));

            // Epoch GlobalEpochId (MinEpochId Offset)*
            
            int epoch = atoi(words[0].c_str());
            LratClauseId globalEpochId = std::stoul(words[1]);
            globalIdStarts.push_back(globalEpochId);
            assert(epoch+1 == globalIdStarts.size());

            std::string out = std::to_string(epoch) + " " + std::to_string(globalEpochId);

            size_t i = 0;
            while (2 + 2*i + 1 < words.size()) {
                while (i >= localIdStartsPerInstance.size()) {
                    localIdStartsPerInstance.emplace_back();
                    localIdOffsetsPerInstance.emplace_back();
                }
                localIdStartsPerInstance.at(i).push_back(std::stoul(words[2+2*i]));
                localIdOffsetsPerInstance.at(i).push_back(std::stoul(words[2+2*i+1]));
                out += " " + std::to_string(localIdStartsPerInstance.at(i).back());
                out += " " + std::to_string(localIdOffsetsPerInstance.at(i).back());
                i++;
            }

            assert(out == line || log_return_false("\"%s\" != \"%s\"\n", out.c_str(), line.c_str()));
        }
        LOG(V2_INFO, "PrAs clause epochs file read\n");

        /*
        // Convert FRAT proofs to LRAT
        std::list<std::future<void>> conversionFutures;
        for (size_t i = 0; i < localIdStartsPerInstance.size(); i++) {
            int instanceId = _this_worker_index * _threads_per_worker + i;
            std::string proofFilenameBase = _params.logDirectory() + "/proof#" 
                + std::to_string(_job_id) + "/proof." + std::to_string(instanceId+1);

            LOG(V2_INFO, "PrAs converting \"%s.frat\" to LRAT\n", proofFilenameBase.c_str());
            std::string conversionCmd = "cat " + proofFilenameBase 
                + ".frat | grep \"a\" | sed 's|a||' | sed 's|l||' > " + proofFilenameBase + ".lrat";
            conversionFutures.push_back(ProcessWideThreadPool::get().addTask([conversionCmd]() {
                int returnCode = system(conversionCmd.c_str());
                assert(returnCode == 0);
            }));
        }
        for (auto& fut : conversionFutures) fut.get();
        LOG(V2_INFO, "PrAs all FRAT2LRAT conversions done\n");
        */

        for (size_t i = 0; i < localIdStartsPerInstance.size(); i++) {
            int instanceId = _this_worker_index * _threads_per_worker + i;
            int numInstances = _num_workers * _threads_per_worker;
            std::string proofFilenameBase = _params.logDirectory() + "/proof#" 
                + std::to_string(_job_id) + "/proof." + std::to_string(instanceId+1);

            _proof_instances.emplace_back(
                instanceId, numInstances, _num_original_clauses, proofFilenameBase + ".lrat", 
                _final_epoch, _winning_instance, globalIdStarts,
                std::move(localIdStartsPerInstance[i]), std::move(localIdOffsetsPerInstance[i]),
                _params.extMemDiskDirectory(), 
                _params.interleaveProofMerging() ? "" : proofFilenameBase + ".filtered.lrat"
            );
        }
    }

    void beginProofAssembly(std::vector<ProofMergeConnector>* conns) {
        int i = 0;
        for (auto& inst : _proof_instances) {
            if (conns) inst.setProofMergeConnector(conns->at(i++));
            inst.advance(nullptr, 0);
        }
        _initialized = true;
    }
};


#pragma once

#include <iomanip>
#include <fstream>
#include <atomic>
#include <cmath>

#include "util/ctre.hpp"

#include "mympi.hpp"
#include "util/hashing.hpp"
#include "util/params.hpp"
#include "util/sys/fileutils.hpp"
#include "comm/sysstate.hpp"
#include "util/sys/proc.hpp"

class HostComm {

private:
    const Parameters& _params;
    MPI_Comm _parent_comm;
    MPI_Comm _comm;

    std::string _base_filename;

    SysState<1>* _sysstate = nullptr;
    std::atomic<float> _ram_usage_this_worker_gb = 0;
    int _active_job_index = -1;
    float _last_contributed_criticality = 0;

public:
    HostComm(MPI_Comm parentComm, const Parameters& params) : _params(params), _parent_comm(parentComm) {}
    ~HostComm() {
        if (_sysstate != nullptr) delete _sysstate;
    }

    void depositInformation() {
        if (_parent_comm == MPI_COMM_NULL) return;
        // Regular layout? -> No deposit of information necessary.
        if (_params.regularProcessDistribution() && _params.processesPerHost() > 0) return;

        // Hash of the program arguments
        auto paramsHash = robin_hood::hash<std::string>()(_params.getParamsAsString());
        std::stringstream hashStream;
        hashStream << std::hex << paramsHash;
        std::string hashString(hashStream.str());
        // Create empty file
        _base_filename = "/tmp/mallob.colleaguerecognition." + hashString + "."; 
        std::ofstream output(_base_filename + std::to_string(MyMpi::rank(_parent_comm)));
    }

    void create() {
        if (_parent_comm == MPI_COMM_NULL) return;
        
        // Determine this rank's color
        int color;
        if (_params.regularProcessDistribution() && _params.processesPerHost() > 0) {
            // Regular layout: Compute color directly from rank and number of processes per host 
            int myRank = MyMpi::rank(_parent_comm);
            color = myRank / _params.processesPerHost();
        } else {
            // List temporary files and extract corresponding ranks
            auto files = FileUtils::glob(_base_filename + "*");
            int nbWorkersThisMachine = files.size();
            static constexpr ctll::fixed_string REGEX_COLLEAGUE_RECOGNITION = 
                ctll::fixed_string{ "(/tmp/mallob\\.colleaguerecognition\\.[0-9a-f]+\\.)([0-9\\.]+)" };
            int minRank = MyMpi::size(MPI_COMM_WORLD);
            for (auto& file : files) {
                auto match = ctre::match<REGEX_COLLEAGUE_RECOGNITION>(file);
                if (match.get<1>().to_string() == _base_filename) {
                    int rank = std::stoi(match.get<2>().to_string());
                    minRank = std::min(minRank, rank);
                }
            }
            color = minRank;
        }

        // Create communicator using the minimum found rank as its "color"
        MPI_Comm_split(_parent_comm, color, MyMpi::rank(_parent_comm), &_comm);

        LOG(V2_INFO, "Machine color %i with %i total workers (my rank: %i)\n", 
            color, MyMpi::size(_comm), MyMpi::rank(_comm));
        
        _sysstate = new SysState<1>(_comm, /*periodSeconds=*/1, MPI_MAX);
    }

    void setRamUsageThisWorkerGbs(float ramGbs) {
        _ram_usage_this_worker_gb = ramGbs;
        updateCriticalityRating();
    }
    void setActiveJobIndex(int index) {
        _active_job_index = index;
        updateCriticalityRating();
    }
    void unsetActiveJobIndex() {
        _active_job_index = -1;
        updateCriticalityRating();
    }

    bool advanceAndCheckMemoryPanic(float time) {
        if (_sysstate == nullptr) return false;

        bool memoryPanic = false;
        bool done = _sysstate->aggregate(time);
        if (done) {

            // Extract result
            float maxCriticality = _sysstate->getGlobal()[0];
            if (maxCriticality == _last_contributed_criticality) {
                // Locally, this worker is the most critical one.
                // Is memory usage too high on this host?
                auto [freeKbs, totalKbs] = Proc::getMachineFreeAndTotalRamKbs();
                auto usedRatio = ((float) freeKbs) / totalKbs;
                if (usedRatio < 0.1) {
                    // Using more than 90% of available RAM!
                    LOG(V3_VERB, "Trigger memory panic (criticality %.4f, %.3f%% mem usage)\n", maxCriticality, usedRatio);
                    memoryPanic = true;
                }
            }

            // Re-set local contribution
            updateCriticalityRating();
        }

        return memoryPanic;
    }

private:
    void updateCriticalityRating() {
        if (_sysstate && !_sysstate->isAggregating())
            _sysstate->setLocal(0, getCriticalityRating());
    }

    float getCriticalityRating() {
        float usedRamGbs = _ram_usage_this_worker_gb.load(std::memory_order_relaxed);
        float indexRating = _active_job_index < 0 ? MyMpi::size(MPI_COMM_WORLD) : _active_job_index+1;
        _last_contributed_criticality = usedRamGbs * sqrt(indexRating);
        return _last_contributed_criticality;
    }

};

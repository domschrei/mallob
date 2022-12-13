
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

    SysState<4>* _sysstate = nullptr;
    const int SYSSTATE_PROCESS_USED_MEMORY = 0;
    const int SYSSTATE_MACHINE_FREE_MEMORY = 1;
    const int SYSSTATE_MACHINE_TOTAL_MEMORY = 2;
    const int SYSSTATE_WORKER_INDEX = 3;  

    std::atomic<float> _ram_usage_this_worker_gb = 0;
    std::atomic<float> _free_machine_memory_kb = 0;
    std::atomic<float> _total_machine_memory_kb = 0;
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
        
        _sysstate = new SysState<4>(_comm, /*periodSeconds=*/1, SysState<4>::ALLGATHER);
    }

    void setRamUsageThisWorkerGbs(float ramGbs) {
        _ram_usage_this_worker_gb = ramGbs;
    }
    void setFreeAndTotalMachineMemoryKbs(unsigned long freeKbs, unsigned long totalKbs) {
        _free_machine_memory_kb = freeKbs;
        _total_machine_memory_kb = totalKbs;
    }
    void setActiveJobIndex(int index) {
        _active_job_index = index;
    }
    void unsetActiveJobIndex() {
        _active_job_index = MyMpi::size(MPI_COMM_WORLD);
    }

    bool advanceAndCheckMemoryPanic(float time) {
        if (_sysstate == nullptr) return false;

        if (_sysstate->canStartAggregating(time)) {
            // Re-set local contribution
            _sysstate->setLocal(SYSSTATE_PROCESS_USED_MEMORY, 1024*1024*_ram_usage_this_worker_gb.load(std::memory_order_relaxed));
            _sysstate->setLocal(SYSSTATE_WORKER_INDEX, _active_job_index);
            _sysstate->setLocal(SYSSTATE_MACHINE_FREE_MEMORY, _free_machine_memory_kb.load(std::memory_order_relaxed));
            _sysstate->setLocal(SYSSTATE_MACHINE_TOTAL_MEMORY, _total_machine_memory_kb.load(std::memory_order_relaxed));
        }

        bool done = _sysstate->aggregate(time);
        if (!done) return false;

        const auto& memoryInformation = _sysstate->getGlobal();

        float machineMinFreeMem = 0;
        float machineMinTotalMem = 0;
        struct ProcessInfo {size_t procIdx; float usedMem; float utility;};
        std::vector<ProcessInfo> processInfo;
        size_t i = 0;
        for (size_t procIdx = 0; procIdx < MyMpi::size(_comm); procIdx++) {
            // Extract data
            float procUsedMem = memoryInformation[i+SYSSTATE_PROCESS_USED_MEMORY];
            float workerIndex = memoryInformation[i+SYSSTATE_WORKER_INDEX];
            float machineFreeMem = memoryInformation[i+SYSSTATE_MACHINE_FREE_MEMORY];
            float machineTotalMem = memoryInformation[i+SYSSTATE_MACHINE_TOTAL_MEMORY];
            i += 4;

            // Update current machine memory
            if (procIdx == 0) {
                machineMinFreeMem = machineFreeMem;
                machineMinTotalMem = machineTotalMem; 
            } else {
                machineMinFreeMem = std::min(machineMinFreeMem, machineFreeMem);
                machineMinTotalMem = std::min(machineMinTotalMem, machineTotalMem);
            }

            // Calculate panic utility for this process 
            processInfo.push_back(ProcessInfo{procIdx, procUsedMem, procUsedMem * (1.0f - (float)std::pow(1.5, -workerIndex))});
        }

        if (machineMinTotalMem <= 0) return false;
        if (machineMinFreeMem / machineMinTotalMem >= 0.0625) return false;
        
        // Panic necessary!
        LOG(V3_VERB, "Memory panic on this machine (%.4f/%.4fGB / %.3f%% free)\n", 
            machineMinFreeMem / 1024.0f / 1024.0f, 
            machineMinTotalMem / 1024.0f / 1024.0f, 
            100 * machineMinFreeMem/machineMinTotalMem);

        // Sort by utility in descending order
        std::sort(processInfo.begin(), processInfo.end(), 
            [&](const auto& left, const auto& right) {return left.utility > right.utility;});
        
        // Iterate over processes and make them panic until enough memory is freed
        i = 0;
        while (i < processInfo.size() && machineMinFreeMem/machineMinTotalMem < 0.125) {
            auto info = processInfo[i];
            if (info.utility <= 0) break;
            machineMinFreeMem += 0.5 * info.usedMem;
            if (info.procIdx == MyMpi::rank(_comm)) {
                LOG(V3_VERB, "Enable memory panic (idx=%lu,usedmem=%.3f,util=%.4f)\n", 
                    info.procIdx, info.usedMem, info.utility);
                // That's me!
                return true;
            }
            i++;
        }

        return false;
    }
};

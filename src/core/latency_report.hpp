
#pragma once

#include "app/job.hpp"
#include "util/data_statistics.hpp"

class LatencyReport {

private:
    std::list<std::vector<float>> _desire_latencies;

public:
    ~LatencyReport() {
        if (_desire_latencies.empty()) return;
        
        // Report statistics on treegrowth ("desire") latencies
        DataStatistics stats(std::move(_desire_latencies));
        stats.computeStats();
        LOG(V3_VERB, "STATS treegrowth_latencies num:%ld min:%.6f max:%.6f med:%.6f mean:%.6f\n", 
            stats.num(), stats.min(), stats.max(), stats.median(), stats.mean());
        stats.logFullDataIntoFile(".treegrowth-latencies");
    }

    void report(Job& job) {
        // Gather statistics
        auto numDesires = job.getJobTree().getNumDesires();
        auto numFulfilledDesires = job.getJobTree().getNumFulfiledDesires();
        auto sumDesireLatencies = job.getJobTree().getSumOfDesireLatencies();
        float desireFulfilmentRatio = numDesires == 0 ? 0 : (float)numFulfilledDesires / numDesires;
        float meanFulfilmentLatency = numFulfilledDesires == 0 ? 0 : sumDesireLatencies / numFulfilledDesires;

        auto& latencies = job.getJobTree().getDesireLatencies();
        float meanLatency = 0, minLatency = 0, maxLatency = 0, medianLatency = 0;
        if (!latencies.empty()) {
            std::sort(latencies.begin(), latencies.end());
            meanLatency = std::accumulate(latencies.begin(), latencies.end(), 0.0f) / latencies.size();
            minLatency = latencies.front();
            maxLatency = latencies.back();
            medianLatency = latencies[latencies.size()/2];
        }

        LOG(V3_VERB, "%s desires fulfilled=%.4f latency={num:%i min:%.5f med:%.5f avg:%.5f max:%.5f}\n",
            job.toStr(), desireFulfilmentRatio, latencies.size(), minLatency, medianLatency, meanLatency, maxLatency);
        
        if (!latencies.empty())
            _desire_latencies.push_back(std::move(latencies));
    }
};

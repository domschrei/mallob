
#include "balancing/simple_balancer.h"
#include "data/job_image.h"

std::map<int, int> SimpleBalancer::balance(std::map<int, JobImage*>& jobs) {

    std::set<int> localJobs;
    for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        JobImage &img = *it->second;
        if ((img.getState() == JobState::ACTIVE) && img.isRoot()) {
            //Console::log("Participating with " + img.toStr() + ", ID " + std::to_string(img.getJob()->getId()));
            JobDescription& job = img.getJob();
            localJobs.insert(job.getId());
        }
    }

    int localSumOfDemands = 0;
    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        localSumOfDemands += getDemand(*jobs[jobId]);
    }

    int globalSumOfAllDemands = allReduce(localSumOfDemands);
    int totalVolume = MyMpi::size(comm);
    
    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        float volumeFloat = (float) totalVolume / globalSumOfAllDemands * getDemand(*jobs[jobId]); 
        volumes[jobId] = std::max(1, (int) std::floor(volumeFloat));
    }

    return volumes;
}
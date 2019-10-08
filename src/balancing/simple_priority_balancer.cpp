
#include "balancing/simple_priority_balancer.h"
#include "data/job_image.h"
#include "util/console.h"

std::map<int, int> SimplePriorityBalancer::balance(std::map<int, JobImage*>& jobs) {

    std::set<int> localJobs;
    for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        JobImage &img = *it->second;
        if ((img.getState() == JobState::ACTIVE) && img.isRoot()) {
            //Console::log("Participating with " + img.toStr() + ", ID " + std::to_string(img.getJob()->getId()));
            Job& job = img.getJob();
            localJobs.insert(job.getId());
        }
    }

    int localSumOfDemands = 0;
    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        int demand = getDemand(jobs[jobId]->getJob());
        Console::log("Demand of #" + std::to_string(jobId) + ": " + std::to_string(demand));
        localSumOfDemands += demand;
    }
    int globalSumOfAllDemands = allReduce(localSumOfDemands);
    
    int localSumOfPriorities = 0;
    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        localSumOfPriorities += jobs[jobId]->getJob().getPriority();
    }
    int globalSumOfAllPriorities = allReduce(localSumOfPriorities);

    int totalVolume = MyMpi::size(comm);
    
    for (auto it = localJobs.begin(); it != localJobs.end(); ++it) {
        int jobId = *it;
        int demand = getDemand(jobs[jobId]->getJob());
        float demandShare = (float) demand / globalSumOfAllDemands;
        float priorityShare = (float) jobs[jobId]->getJob().getPriority() / globalSumOfAllPriorities;
        int permittedVolume = std::max(1, (int) std::floor(demandShare * priorityShare * totalVolume));
        volumes[jobId] = std::min(permittedVolume, demand);
    }

    return volumes;
}
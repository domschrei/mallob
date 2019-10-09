
#include "thermodynamic_balancer.h"
#include "util/console.h"

std::map<int, int> ThermodynamicBalancer::balance(std::map<int, JobImage*>& jobs) {

    std::vector<JobDescription*> activeJobs;
    std::vector<JobDescription*> involvedJobs;
    std::map<int, float> newVolumes;
    for (auto it = jobs.begin(); it != jobs.end(); ++it) {
        JobImage &img = *it->second;
        if ((img.getState() == JobState::ACTIVE) && img.isRoot()) {
            //Console::log("Participating with " + img.toStr() + ", ID " + std::to_string(img.getJob()->getId()));
            JobDescription& job = img.getJob();

            assert(getTemperature(job.getId()) > 0);
            assert(job.getPriority() > 0);

            activeJobs.push_back(&job);
            involvedJobs.push_back(&job);
            newVolumes[job.getId()] = 0;
        }
    }

    int fullVolume = MyMpi::size(comm);
    Console::log(std::to_string(fullVolume));
    Console::log(std::to_string(loadFactor));

    // Initial pressure, filtering out micro-jobs
    float remainingVolume = fullVolume * loadFactor;
    assert(remainingVolume > 0);
    float pressure = calculatePressure(involvedJobs, remainingVolume);
    assert(pressure >= 0);
    float microJobDiscount = 0;
    for (unsigned int i = 0; i < involvedJobs.size(); i++) {
        const JobDescription& job = *involvedJobs[i];
        float demand = 1/pressure * getTemperature(job.getId()) * job.getPriority();
        if (demand < 1) {
            // Micro job!
            microJobDiscount++;
            newVolumes[job.getId()] = 1;
            involvedJobs.erase(involvedJobs.begin() + i);
            i--;
        }
    }
    remainingVolume -= allReduce(microJobDiscount);
    assert(remainingVolume >= 0);

    // Main iterations for computing exact job demands
    int iteration = 0;
    while (remainingVolume >= 1 && iteration <= 10) {
        float unusedVolume = 0;
        pressure = calculatePressure(involvedJobs, remainingVolume);
        if (pressure == 0) break;
        //if (MyMpi::rank(comm) == 0) Console::log("Pressure: " + std::to_string(pressure));
        for (unsigned int i = 0; i < involvedJobs.size(); i++) {
            JobDescription& job = *involvedJobs[i];
            float addition = 1/pressure * getTemperature(job.getId()) * job.getPriority();
            //Console::log(std::to_string(pressure) + "," + std::to_string(job->getTemperature()) + "," + std::to_string(job->getPriority()));
            float upperBound = getDemand(*jobs[job.getId()]);
            float demand = newVolumes[job.getId()];
            if (demand + addition >= upperBound) {
                // Upper bound hit
                unusedVolume += (demand + addition) - upperBound;
                addition = upperBound - demand;
                involvedJobs.erase(involvedJobs.begin() + i);
                i--;
            }
            assert(addition >= 0);
            newVolumes[job.getId()] += addition;
        }
        remainingVolume = allReduce(unusedVolume);
        iteration++;
    }
    if (MyMpi::rank(comm) == 0)
        Console::log("Did " + std::to_string(iteration) + " rebalancing iterations");

    // Weigh remaining volume against shrinkage
    float shrink = 0;
    for (auto it = activeJobs.begin(); it != activeJobs.end(); ++it) {
        JobDescription& job = **it;
        assert(newVolumes[job.getId()] >= 0);
        if (getVolume(job.getId()) - newVolumes[job.getId()] > 0) {
            shrink += getVolume(job.getId()) - newVolumes[job.getId()];
        }
    }
    float shrinkage = allReduce(shrink);
    float shrinkageFactor;
    if (shrinkage == 0) {
        shrinkageFactor = 0;
    } else {
        shrinkageFactor = (shrinkage - remainingVolume) / shrinkage;
    }
    assert(shrinkageFactor >= 0);
    assert(shrinkageFactor <= 1);

    // Round and apply volume updates
    int allDemands = 0;
    for (auto it = activeJobs.begin(); it != activeJobs.end(); ++it) {

        JobDescription& job = **it;
        float delta = newVolumes[job.getId()] - getVolume(job.getId());

        if (delta < 0) {
            delta = delta * shrinkageFactor;
        }
        // Probabilistic rounding
        /*
        float random = Random::rand();
        if (random < delta - std::floor(delta)) {
            delta = std::ceil(delta);
        } else {*/
            delta = std::floor(delta);
        //}

        volumes[job.getId()] = getVolume(job.getId()) + delta;
        allDemands += volumes[job.getId()];
    }

    return volumes;
}

float ThermodynamicBalancer::calculatePressure(const std::vector<JobDescription*>& involvedJobs, float volume) {

    float contribution = 0;
    for (auto it = involvedJobs.begin(); it != involvedJobs.end(); ++it) {
        const JobDescription& job = **it;
        contribution += getTemperature(job.getId()) * job.getPriority();
    }
    float allContributions = allReduce(contribution);
    float pressure = allContributions / volume;
    return pressure;
}
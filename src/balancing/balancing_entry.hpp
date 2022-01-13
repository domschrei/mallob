
#ifndef DOMPASCH_MALLOB_BALANCING_ENTRY_HPP
#define DOMPASCH_MALLOB_BALANCING_ENTRY_HPP

#include <cmath>

struct BalancingEntry {

    int jobId;
    int demand;
    int originalDemand;
    float priority;

    double fairShare;
    int volumeLower;
    int volume;
    int volumeUpper;

    BalancingEntry(int jobId, int demand, float priority) : jobId(jobId), demand(demand), priority(priority) {
        originalDemand = demand;
        volumeLower = 1;
        volume = 1;
        volumeUpper = 1;
    }

    inline int getVolume(double fairShareMultiplier) const {
        
        int fairVolume;
        // Overflow protection
        if (fairShareMultiplier >= demand && fairShare >= 1) fairVolume = demand;
        else if (fairShareMultiplier >= 1 && fairShare >= demand) fairVolume = demand;
        else fairVolume = (int) (fairShareMultiplier * fairShare); // FLOOR
        
        return std::max(1, std::min(demand, fairVolume));
    }

    inline double getFairShareMultiplierLowerBound() const {
        return 1 / fairShare;
    }

    inline double getFairShareMultiplierUpperBound() const {
        return demand / fairShare;
    }

    void dismissAndSwapWith(BalancingEntry& other) {
        
        // Copy base fields of this (dismissed) entry
        int tmpJobId = jobId;
        int tmpDemand = demand;
        float tmpPriority = priority;
        float tmpFairShare = fairShare;
        int tmpVolume = volume;

        // Copy ALL fields of other (active) entry to this object
        jobId = other.jobId;
        demand = other.demand;
        priority = other.priority;
        fairShare = other.fairShare;
        volume = other.volume;
        volumeLower = other.volumeLower;
        volumeUpper = other.volumeUpper;

        // Copy base fields of dismissed entry to other object
        other.jobId = tmpJobId;
        other.demand = tmpDemand;
        other.priority = tmpPriority;
        other.fairShare = tmpFairShare;
        other.volume = tmpVolume;
    }
};

#endif

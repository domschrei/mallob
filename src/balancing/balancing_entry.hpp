
#ifndef DOMPASCH_MALLOB_BALANCING_ENTRY_HPP
#define DOMPASCH_MALLOB_BALANCING_ENTRY_HPP

#include <cmath>

struct BalancingEntry {

    int jobId;
    int demand;
    float priority;

    double fairShare;
    double remainder;
    int volumeLower;
    int volume;
    int volumeUpper;

    BalancingEntry(int jobId, int demand, float priority) : jobId(jobId), demand(demand), priority(priority) {
        volumeLower = 1;
        volume = 1;
        volumeUpper = 1;
    }

    inline int getVolume(double fairShareMultiplier) const {
        return std::max(1, std::min(demand, (int)(fairShareMultiplier * fairShare + 1 - remainder)));
    }

    inline double getFairShareMultiplierLowerBound() const {
        constexpr int EPSILON = 0.0001;
        return (remainder + 1 - EPSILON) / fairShare;
    }

    inline double getFairShareMultiplierUpperBound() const {
        constexpr int EPSILON = 0.0001;
        return (demand + remainder - 1) / fairShare + EPSILON;
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
        remainder = other.remainder;
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

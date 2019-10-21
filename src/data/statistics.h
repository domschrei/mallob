
#ifndef DOMPASCH_BALANCER_STATISTICS_H
#define DOMPASCH_BALANCER_STATISTICS_H

#include <vector>
#include <map>

#include "data/epoch_counter.h"
#include "data/reduceable.h"

typedef std::map<const char*, float> AtomicStatisticsMap;
typedef std::map<const char*, std::vector<float>> VectorStatisticsMap;

class Statistics {
private:
    AtomicStatisticsMap globalAtomics;
    VectorStatisticsMap globalVectors;

    const EpochCounter& epochCounter;
    std::vector<AtomicStatisticsMap> epochAtomics;
    std::vector<VectorStatisticsMap> epochVectors;

public:
    Statistics(const EpochCounter& epochCounter) : epochCounter(epochCounter) {
        globalAtomics = initAtomics();
        globalVectors = initVectors();
    }

    // atomic statistics
    void increment(const char* tag);
    void set(const char* tag, float num);
    void add(const char* tag, float amount);

    // vector statistics
    void push_back(const char* tag, float num);

    void dump();
    void dump(uint epoch);
    void dump(uint minEpoch, uint maxEpoch, bool dumpGlobalStats);

private:
    AtomicStatisticsMap& currentAtomics();
    VectorStatisticsMap& currentVectors();
    AtomicStatisticsMap initAtomics();
    VectorStatisticsMap initVectors();

    void print(AtomicStatisticsMap& atomics);
    void print(VectorStatisticsMap& vectors);
};

#endif
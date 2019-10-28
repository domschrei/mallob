
#include "statistics.h"

#include "util/console.h"

AtomicStatisticsMap Statistics::initAtomics() {
    AtomicStatisticsMap atomics;
    atomics["receivedMessages"] = 0;
    atomics["sentMessages"] = 0;
    atomics["idleTime"] = 0;
    atomics["busyTime"] = 0;
    atomics["bouncedJobs"] = 0;
    atomics["reductions"] = 0;
    atomics["broadcasts"] = 0;
    return atomics;
}

VectorStatisticsMap Statistics::initVectors() {
    VectorStatisticsMap vectors;
    vectors["bounces"] = std::vector<float>();
    return vectors;
}

AtomicStatisticsMap& Statistics::currentAtomics() {
    while (epochCounter.getEpoch() >= epochAtomics.size())
        epochAtomics.push_back(initAtomics());
    return epochAtomics[epochCounter.getEpoch()];
}

VectorStatisticsMap& Statistics::currentVectors() {
    while (epochCounter.getEpoch() >= epochVectors.size())
        epochVectors.push_back(initVectors());
    return epochVectors[epochCounter.getEpoch()];
}

void Statistics::set(const char* tag, float num) {
    assert(globalAtomics.count(tag));
    globalAtomics[tag] = num;
    currentAtomics()[tag] = num;
}

void Statistics::add(const char* tag, float amount) {
    assert(globalAtomics.count(tag));
    globalAtomics[tag] += amount;
    currentAtomics()[tag] += amount;
}

void Statistics::increment(const char* tag) {
    assert(globalAtomics.count(tag));
    globalAtomics[tag]++;
    currentAtomics()[tag]++;
}

void Statistics::push_back(const char* tag, float num) {
    assert(globalVectors.count(tag));
    globalVectors[tag].push_back(num);
    currentVectors()[tag].push_back(num);
}

void Statistics::dump() {
    dump(0, epochCounter.getEpoch(), true);
}

void Statistics::dump(int epoch) {
    dump(epoch, epoch, false);
}

void Statistics::dump(int minEpoch, int maxEpoch, bool dumpGlobalStats) {
    
    Console::getLock();

    if (dumpGlobalStats) {
        Console::appendUnsafe(Console::INFO, "ALL_EPOCHS ");
        print(globalAtomics);
        print(globalVectors);
        Console::logUnsafe(Console::INFO, ""); // new line
    }

    for (size_t epoch = minEpoch; epoch <= maxEpoch; epoch++) {
        Console::appendUnsafe(Console::INFO, "EPOCH_%i ", epoch);
        if (epoch >= epochAtomics.size()) 
            epochAtomics.push_back(initAtomics());
        print(epochAtomics[epoch]);
        if (epoch >= epochVectors.size())
            epochVectors.push_back(initVectors());
        print(epochVectors[epoch]);
        Console::logUnsafe(Console::INFO, ""); // new line
    }

    Console::releaseLock();
}

void Statistics::print(AtomicStatisticsMap& atomics) {
    for (auto it : atomics) {
        const char* tag = it.first;
        Console::appendUnsafe(Console::INFO, "%s=%3.3f ", tag, it.second);
    }
}

void Statistics::print(VectorStatisticsMap& vectors) {
    for (auto it : vectors) {
        const char* tag = it.first;
        Console::appendUnsafe(Console::INFO, "%s=[ ", tag);
        std::vector<float> vec = it.second;
        for (auto iv : vec) {
            Console::appendUnsafe(Console::INFO, "%3.3f ", iv);
        }
        Console::appendUnsafe(Console::INFO, "] ");
    }
}
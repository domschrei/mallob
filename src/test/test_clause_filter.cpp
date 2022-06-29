
#include <bitset>

#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/random.hpp"
#include "util/sys/process.hpp"

#include "app/sat/sharing/filter/clause_filter.hpp"
#include "util/atomic_bitset/atomic_wide_bitset.hpp"
#include "util/atomic_bitset/atomic_bitset.hpp"

void test() {

    int numSets = 1000000;
    {
        double time = Timer::elapsedSeconds();
        std::vector<AtomicWideBitset*> filters;
        for (size_t i = 0; i < 1; i++) filters.push_back(new AtomicWideBitset(NUM_BITS));
        time = Timer::elapsedSeconds() - time;
        auto info = Proc::getRuntimeInfo(Proc::getPid(), Proc::FLAT);
        LOG(V2_INFO, "AtomicWideBitset initialization took %.5fs, RSS %.3f\n", time, info.residentSetSize);
        
        time = Timer::elapsedSeconds();
        for (size_t i = 0; i < numSets; i++) {
            filters.back()->set((int) (Random::rand()*NUM_BITS));
        }
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "AtomicWideBitset set took %.8fs\n", time/numSets);

        time = Timer::elapsedSeconds();
        filters.back()->reset();
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "AtomicWideBitset reset took %.5fs\n", time);
        
        delete filters.back();
    }
    {
        double time = Timer::elapsedSeconds();
        std::vector<atomicbitvector::atomic_bv_t*> filters;
        for (size_t i = 0; i < 1; i++) filters.push_back(new atomicbitvector::atomic_bv_t(NUM_BITS));
        time = Timer::elapsedSeconds() - time;
        auto info = Proc::getRuntimeInfo(Proc::getPid(), Proc::FLAT);
        LOG(V2_INFO, "atomicbitvector::atomic_bv_t initialization took %.5fs, RSS %.3f\n", time, info.residentSetSize);
        time = Timer::elapsedSeconds();
        for (size_t i = 0; i < numSets; i++) {
            filters.back()->set((int) (Random::rand()*NUM_BITS));
        }
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "atomicbitvector::atomic_bv_t set took %.8fs\n", time/numSets);

        time = Timer::elapsedSeconds();
        filters.back()->reset();
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "atomicbitvector::atomic_bv_t reset took %.5fs\n", time);
        
        delete filters.back();
    }
    {
        double time = Timer::elapsedSeconds();
        std::vector<std::bitset<NUM_BITS>*> filters;
        for (size_t i = 0; i < 1; i++) filters.push_back(new std::bitset<NUM_BITS>());
        time = Timer::elapsedSeconds() - time;
        auto info = Proc::getRuntimeInfo(Proc::getPid(), Proc::FLAT);
        LOG(V2_INFO, "std::bitset initialization took %.5fs, RSS %.3f\n", time, info.residentSetSize);
        time = Timer::elapsedSeconds();
        for (size_t i = 0; i < numSets; i++) {
            filters.back()->set((int) (Random::rand()*NUM_BITS));
        }
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "std::bitset set took %.8fs\n", time/numSets);

        time = Timer::elapsedSeconds();
        filters.back()->reset();
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, "std::bitset reset took %.5fs\n", time);
        
        delete filters.back();
    }
}


int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);

    test();
}


#include "util/sys/timer.hpp"
#include "util/random.hpp"

#include "balancing/volume_calculator.hpp"

double runtime = 0;

void testFunction(Parameters& params) {
    BalancingEntry entry(1, 10, 1);
    entry.fairShare = 5;

    std::vector<double> multipliers;
    multipliers.push_back(1.0 / entry.fairShare);
    multipliers.push_back(1);
    multipliers.push_back(entry.demand / entry.fairShare);

    for (double x : multipliers) {
        LOG(V2_INFO, "x=%.4f cont_v(x)=%.4f v_j(x)=%i\n", x, entry.fairShare*x, entry.getVolume(x));
    }
}

std::vector<BalancingEntry> testEventMap(Parameters& params, EventMap& map, int numWorkers, int expectedUtilization) {
    int sum = 0;
    runtime = Timer::elapsedSeconds();
    VolumeCalculator calc(map, params, numWorkers, V4_VVER);
    calc.calculateResult();
    runtime = Timer::elapsedSeconds() - runtime;
    bool allDemandsMet = true;
    for (const auto& entry : calc.getEntries()) {
        assert(entry.volume <= entry.demand);
        assert(entry.demand == 0 || entry.volume >= 1);
        sum += entry.volume;
        allDemandsMet = allDemandsMet && entry.volume == entry.originalDemand;
    }
    for (const auto& entry : calc.getEntries()) {
        LOG(V5_DEBG, "  #%i : fair share %.4f ~> volume %i\n", entry.jobId, entry.fairShare, entry.volume);
    }
    assert(allDemandsMet || sum == expectedUtilization || LOG_RETURN_FALSE("%i != %i\n", sum, expectedUtilization));
    return calc.getEntries();
}

void testUniformUnderutilization(Parameters& params) {
    LOG(V2_INFO, "#### Test uniform underutilization ####\n");
    EventMap map;
    // ID, epoch, demand, priority
    map.insertIfNovel(Event({/*ID=*/1, /*epoch=*/1, /*demand=*/10, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/2, /*epoch=*/1, /*demand=*/10, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/3, /*epoch=*/1, /*demand=*/10, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/4, /*epoch=*/1, /*demand=*/10, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/5, /*epoch=*/1, /*demand=*/10, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/6, /*epoch=*/1, /*demand=*/10, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/7, /*epoch=*/1, /*demand=*/10, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/8, /*epoch=*/1, /*demand=*/10, /*priority=*/1}));

    auto result = testEventMap(params, map, /*numWorkers=*/100, /*expectedUtilization=*/80);
    for (const auto& entry : result) {
        assert(entry.volume == 10);
    }
}

void testUniform(Parameters& params) {
    LOG(V2_INFO, "#### Test uniform ####\n");
    EventMap map;
    // ID, epoch, demand, priority
    map.insertIfNovel(Event({/*ID=*/1, /*epoch=*/1, /*demand=*/10000, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/2, /*epoch=*/1, /*demand=*/10000, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/3, /*epoch=*/1, /*demand=*/10000, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/4, /*epoch=*/1, /*demand=*/10000, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/5, /*epoch=*/1, /*demand=*/10000, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/6, /*epoch=*/1, /*demand=*/10000, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/7, /*epoch=*/1, /*demand=*/10000, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/8, /*epoch=*/1, /*demand=*/10000, /*priority=*/1}));

    auto result = testEventMap(params, map, /*numWorkers=*/100, /*expectedUtilization=*/100);
    for (const auto& entry : result) {
        assert(entry.volume == 12 || entry.volume == 13);
    }
}

void testSimilarPriorities(Parameters& params) {
    LOG(V2_INFO, "#### Test similar priorities ####\n");
    EventMap map;
    // ID, epoch, demand, priority
    map.insertIfNovel(Event({/*ID=*/1, /*epoch=*/1, /*demand=*/10000, /*priority=*/0.501}));
    map.insertIfNovel(Event({/*ID=*/2, /*epoch=*/1, /*demand=*/10000, /*priority=*/0.502}));
    map.insertIfNovel(Event({/*ID=*/3, /*epoch=*/1, /*demand=*/10000, /*priority=*/0.503}));
    map.insertIfNovel(Event({/*ID=*/4, /*epoch=*/1, /*demand=*/10000, /*priority=*/0.504}));
    map.insertIfNovel(Event({/*ID=*/5, /*epoch=*/1, /*demand=*/10000, /*priority=*/0.505}));
    map.insertIfNovel(Event({/*ID=*/6, /*epoch=*/1, /*demand=*/10000, /*priority=*/0.506}));
    map.insertIfNovel(Event({/*ID=*/7, /*epoch=*/1, /*demand=*/10000, /*priority=*/0.507}));
    map.insertIfNovel(Event({/*ID=*/8, /*epoch=*/1, /*demand=*/10000, /*priority=*/0.508}));

    auto result = testEventMap(params, map, /*numWorkers=*/100, /*expectedUtilization=*/100);

    for (const auto& entry : result) {
        assert(entry.volume == 12 || entry.volume == 13);
        if (entry.volume == 12) assert(entry.priority <= 0.504f);
        if (entry.volume == 13) assert(entry.priority >= 0.505f);
    }
}

void testPerformance(Parameters& params) {
    LOG(V2_INFO, "#### Test performance ####\n");

    float minPriority = 0.001;
    int numWorkers = 1 << 23;

    std::vector<int> numJobsVec;
    for (float i = 64; i <= 1 << 20; i*=2) {
        if (i <= numWorkers) numJobsVec.push_back((int)std::ceil(i));
    }
    //for (int i = 10000; i <= 1e6; i+=10000) for (int rep = 1; rep <= numReps; rep++) numJobsVec.push_back(i);
    
    std::map<int, int> numRuntimesPerSize;
    std::map<int, float> runtimesPerSize;

    for (int rep = 1; rep <= 3; rep++) {
        for (int i = 0; i < numJobsVec.size(); i++) {
            int numJobs = numJobsVec[i];

            int maxDemand = numWorkers - numJobs + 1;
            //float log2MaxDemand = log2(maxDemand);

            EventMap map;
            unsigned long long sumOfDemands = 0;
            for (size_t i = 0; i < numJobs; i++) {
                //int demand = (int) std::round(std::pow(2, log2MaxDemand*Random::rand()));
                int demand = (int) std::round(1 + Random::rand() * (maxDemand-1));
                Event ev{/*ID=*/(int)(i+1), /*epoch=*/1, /*demand=*/demand, 
                    /*priority=*/minPriority+(1-minPriority)*Random::rand()};
                assert(ev.demand >= 1);
                assert(ev.demand <= maxDemand);
                assert(ev.priority > minPriority);
                assert(ev.priority <= 1);
                map.insertIfNovel(ev);
                sumOfDemands += ev.demand;
            }
                
            LOG(V2_INFO, "nJobs=%i nWorkers=%i minPriority=%.6f maxDemand=%i sumOfDemands=%llu\n", 
                numJobs, numWorkers, minPriority, maxDemand, sumOfDemands);

            auto result = testEventMap(params, map, numWorkers, /*expectedUtilization=*/numWorkers);
            numRuntimesPerSize[numJobs]++;
            runtimesPerSize[numJobs] += runtime;
        }
    }

    float totalTime = 0;
    for (auto& [numJobs, runtimes] : runtimesPerSize) {
        totalTime += runtimes;
        LOG(V2_INFO, "nJobs=%i avgTime=%.6fs\n", numJobs, runtimes/numRuntimesPerSize[numJobs]);
    }
    LOG(V2_INFO, "Total time: %.5fs\n", totalTime);
}

void testSmall(Parameters& params) {
    LOG(V2_INFO, "#### Test small ####\n");
    EventMap map;
    map.insertIfNovel(Event({/*ID=*/1, /*epoch=*/1, /*demand=*/10, /*priority=*/0.1}));
    map.insertIfNovel(Event({/*ID=*/2, /*epoch=*/1, /*demand=*/10, /*priority=*/0.05}));
    map.insertIfNovel(Event({/*ID=*/3, /*epoch=*/1, /*demand=*/10, /*priority=*/0.025}));

    auto result = testEventMap(params, map, /*numWorkers=*/15, /*expectedUtilization=*/15);
}

void testDivergentDemandPriorityRatio(Parameters& params) {
    LOG(V2_INFO, "#### Test divergent demand/priority ratio ####\n");
    EventMap map;
    map.insertIfNovel(Event({/*ID=*/1, /*epoch=*/1, /*demand=*/128, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/2, /*epoch=*/1, /*demand=*/64, /*priority=*/2}));
    map.insertIfNovel(Event({/*ID=*/3, /*epoch=*/1, /*demand=*/32, /*priority=*/3}));
    map.insertIfNovel(Event({/*ID=*/4, /*epoch=*/1, /*demand=*/16, /*priority=*/4}));
    map.insertIfNovel(Event({/*ID=*/5, /*epoch=*/1, /*demand=*/8, /*priority=*/5}));
    map.insertIfNovel(Event({/*ID=*/6, /*epoch=*/1, /*demand=*/4, /*priority=*/6}));
    map.insertIfNovel(Event({/*ID=*/7, /*epoch=*/1, /*demand=*/2, /*priority=*/7}));
    map.insertIfNovel(Event({/*ID=*/8, /*epoch=*/1, /*demand=*/1, /*priority=*/8}));

    auto result = testEventMap(params, map, /*numWorkers=*/40, /*expectedUtilization=*/40);
}

void testConvergentDemandPriorityRatio(Parameters& params) {
    LOG(V2_INFO, "#### Test convergent demand/priority ratio ####\n");
    EventMap map;
    map.insertIfNovel(Event({/*ID=*/1, /*epoch=*/1, /*demand=*/1, /*priority=*/1}));
    map.insertIfNovel(Event({/*ID=*/2, /*epoch=*/1, /*demand=*/2, /*priority=*/2}));
    map.insertIfNovel(Event({/*ID=*/3, /*epoch=*/1, /*demand=*/3, /*priority=*/3}));
    map.insertIfNovel(Event({/*ID=*/4, /*epoch=*/1, /*demand=*/4, /*priority=*/4}));
    map.insertIfNovel(Event({/*ID=*/5, /*epoch=*/1, /*demand=*/5, /*priority=*/5}));
    map.insertIfNovel(Event({/*ID=*/6, /*epoch=*/1, /*demand=*/6, /*priority=*/6}));
    map.insertIfNovel(Event({/*ID=*/7, /*epoch=*/1, /*demand=*/7, /*priority=*/7}));
    map.insertIfNovel(Event({/*ID=*/8, /*epoch=*/1, /*demand=*/8, /*priority=*/8}));

    auto result = testEventMap(params, map, /*numWorkers=*/20, /*expectedUtilization=*/20);
}

void testHugeModifier(Parameters& params) {
    LOG(V2_INFO, "#### Test Huge Modifier ####\n");
    EventMap map;
    map.insertIfNovel(Event({/*ID=*/1, /*epoch=*/1, /*demand=*/2, /*priority=*/1000}));
    map.insertIfNovel(Event({/*ID=*/2, /*epoch=*/1, /*demand=*/100000, /*priority=*/1}));

    auto result = testEventMap(params, map, /*numWorkers=*/100000, /*expectedUtilization=*/100000);
}

void testTinyModifier(Parameters& params) {
    LOG(V2_INFO, "#### Test Tiny Modifier ####\n");
    EventMap map;
    for (int j = 0; j < 97; j++)
        map.insertIfNovel(Event({/*ID=*/j+1, /*epoch=*/1, /*demand=*/100, /*priority=*/0.01}));
    map.insertIfNovel(Event({/*ID=*/98, /*epoch=*/1, /*demand=*/100, /*priority=*/0.5}));
    map.insertIfNovel(Event({/*ID=*/99, /*epoch=*/1, /*demand=*/100, /*priority=*/0.5}));

    auto result = testEventMap(params, map, /*numWorkers=*/100, /*expectedUtilization=*/100);
}

int main(int argc, char *argv[]) {
    Timer::init();
    Parameters params;
    params.init(argc, argv);
    Random::init(params.seed(), params.seed());

    Logger::init(0, params.verbosity());

    testFunction(params);
    testUniformUnderutilization(params);
    testUniform(params);
    testSimilarPriorities(params);
    testSmall(params);
    testConvergentDemandPriorityRatio(params);
    testDivergentDemandPriorityRatio(params);
    testTinyModifier(params);
    testHugeModifier(params);
    testPerformance(params);
}



#include "util/sys/timer.hpp"
#include "util/random.hpp"

#include "balancing/volume_calculator.hpp"

void testFunction(Parameters& params) {
    BalancingEntry entry(1, 10, 1);
    entry.fairShare = 5;
    entry.remainder = 0.4;

    std::vector<double> multipliers;
    multipliers.push_back(1.0 / entry.fairShare);
    multipliers.push_back(1);
    multipliers.push_back(entry.demand / entry.fairShare);

    for (double x : multipliers) {
        log(V2_INFO, "x=%.4f cont_v(x)=%.4f v_j(x)=%i\n", x, entry.fairShare*x, entry.getVolume(x));
    }
}

std::vector<BalancingEntry> testEventMap(Parameters& params, EventMap& map, int numWorkers, int expectedUtilization) {
    int sum = 0;
    VolumeCalculator calc(map, params, numWorkers, 1);
    calc.calculateResult();
    for (const auto& entry : calc.getEntries()) {
        assert(entry.volume <= entry.demand);
        assert(entry.demand == 0 || entry.volume >= 1);
        sum += entry.volume;
    }
    assert(sum == expectedUtilization || log_return_false("%i != %i\n", sum, expectedUtilization));
    return calc.getEntries();
}

void test1(Parameters& params) {
    log(V2_INFO, "#### Test 1 ####\n");
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

void test2(Parameters& params) {
    log(V2_INFO, "#### Test 2 ####\n");
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

void test3(Parameters& params) {
    log(V2_INFO, "#### Test 3 ####\n");
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
        log(V2_INFO, "  #%i : %i\n", entry.jobId, entry.volume);
    }
    for (const auto& entry : result) {
        assert(entry.volume == 12 || entry.volume == 13);
        if (entry.volume == 12) assert(entry.priority <= 0.504f);
        if (entry.volume == 13) assert(entry.priority >= 0.505f);
    }
}

void test4(Parameters& params) {
    log(V2_INFO, "#### Test 4 ####\n");

    std::vector<int> numJobsVec;
    for (int i = 1; i < 1 << 20; i*=2) numJobsVec.push_back(i);
    //numJobsVec.push_back(1000);
    std::vector<float> times;
    
    for (int numJobs : numJobsVec) {
        log(V2_INFO, "nJobs = %i\n", numJobs);
        float time = Timer::elapsedSeconds();

        EventMap map;
        for (size_t i = 0; i < numJobs; i++) {
            map.insertIfNovel(Event({/*ID=*/(int)(i+1), /*epoch=*/1, /*demand=*/(int)(1+Random::rand()*99), /*priority=*/Random::rand()}));
        }

        auto result = testEventMap(params, map, /*numWorkers=*/numJobs*10, /*expectedUtilization=*/numJobs*10);
        time = Timer::elapsedSeconds() - time;
        times.push_back(time);
    }

    float totalTime = 0;
    for (size_t i = 0; i < numJobsVec.size(); i++) {
        totalTime += times[i];
        log(V2_INFO, "nJobs=%i time=%.5fs\n", numJobsVec[i], times[i]);
    }
    log(V2_INFO, "Total time: %.5fs\n", totalTime);
}

void test5(Parameters& params) {
    log(V2_INFO, "#### Test 5 ####\n");
    EventMap map;
    map.insertIfNovel(Event({/*ID=*/1, /*epoch=*/1, /*demand=*/10, /*priority=*/0.1}));
    map.insertIfNovel(Event({/*ID=*/2, /*epoch=*/1, /*demand=*/10, /*priority=*/0.05}));
    map.insertIfNovel(Event({/*ID=*/3, /*epoch=*/1, /*demand=*/10, /*priority=*/0.025}));

    auto result = testEventMap(params, map, /*numWorkers=*/15, /*expectedUtilization=*/15);
    for (const auto& entry : result) {
        log(V2_INFO, "  #%i : fair share %.4f ~> volume %i\n", entry.jobId, entry.fairShare, entry.volume);
    }
}

void test6(Parameters& params) {
    log(V2_INFO, "#### Test 6 ####\n");
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
    for (const auto& entry : result) {
        log(V2_INFO, "  #%i : fair share %.4f ~> volume %i\n", entry.jobId, entry.fairShare, entry.volume);
    }
}

void test7(Parameters& params) {
    log(V2_INFO, "#### Test 7 ####\n");
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
    for (const auto& entry : result) {
        log(V2_INFO, "  #%i : fair share %.4f ~> volume %i\n", entry.jobId, entry.fairShare, entry.volume);
    }
}

int main(int argc, char *argv[]) {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG, false, false, false, nullptr);

    Parameters params;
    params.init(argc, argv);

    testFunction(params);
    test1(params);
    test2(params);
    test3(params);
    test4(params);
    test5(params);
    test6(params);
    test7(params);
}



#include <memory.h>
#include <unistd.h>

#include "util/sys/timer.hpp"
#include "util/params.hpp"
#include "util/random.hpp"
#include "util/logger.hpp"

#include <vector>
#include <thread>
#include <list>

void testConcurrentAllocation() {

    const int nThreads = 128;
    const int nAllocs = 100;

    std::vector<std::thread> threads;
    std::vector<std::list<void*>> allocatedMemory;

    float time = Timer::elapsedSeconds();

    allocatedMemory.resize(nThreads);
    for (int i = 0; i < nThreads; i++) {
        threads.emplace_back([nThreads, i, &allocatedMemory]() {
            usleep(10 * (nThreads-i));
            log(V2_INFO, "Thread #%i: alloc\n", i);
            // allocate
            for (int j = 0; j < nAllocs; j++) {
                size_t memsize = 100'000 + Random::rand() * 99'900'000;
                allocatedMemory[i].push_back(malloc(memsize));
                usleep(1000 * 100 * Random::rand());
            }
            log(V2_INFO, "Thread #%i: free\n", i);
            // free
            for (void* mem : allocatedMemory[i]) {
                free(mem);
                usleep(1000 * 100 * Random::rand());
            }
            log(V2_INFO, "Thread #%i: done\n", i);
        });
    }

    for (auto& thread : threads) thread.join();
    
    time = Timer::elapsedSeconds() - time;
    log(V2_INFO, "Time: %.6fs\n", time);
}

int main(int argc, char *argv[]) {
    Timer::init();
    Parameters params;
    params.init(argc, argv);
    Random::init(params.seed(), params.seed());

    Logger::init(0, params.verbosity(), false, false, false, nullptr);

    testConcurrentAllocation();
}

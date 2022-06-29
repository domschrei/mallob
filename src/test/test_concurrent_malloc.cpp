
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

    const int nThreads = 4;
    const int nAllocs = 10000;

    std::vector<std::thread> threads;

    float time = Timer::elapsedSeconds();

    for (int i = 0; i < nThreads; i++) {
        threads.emplace_back([nThreads, i]() {
            LOG(V2_INFO, "Thread #%i: alloc+free\n", i);
            // allocate
            std::list<void*> memory;
            for (int j = 0; j < nAllocs; j++) {
                size_t memsize = 1'000'000+1'000*j;
                void* mem = malloc(memsize);
                memory.push_back(mem);
                if (memory.size() >= 100) {
                    free(memory.front());
                    memory.pop_front();
                }
            }
            while (!memory.empty()) {
                free(memory.front());
                memory.pop_front();
            }
            LOG(V2_INFO, "Thread #%i: done\n", i);
        });
    }

    for (auto& thread : threads) thread.join();
    
    time = Timer::elapsedSeconds() - time;
    LOG(V2_INFO, "Time: %.6fs\n", time);
}

int main(int argc, char *argv[]) {
    Timer::init();
    Parameters params;
    params.init(argc, argv);
    Random::init(params.seed(), params.seed());

    Logger::init(0, params.verbosity());

    testConcurrentAllocation();
}

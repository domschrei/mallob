
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>
#include <thread>
#include <set>
#include <random>
#include <unistd.h>

#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/terminator.hpp"
#include "util/external_priority_queue.hpp"

void testBasic() {
    
    ExternalPriorityQueue<unsigned long> q;

    LOG(V2_INFO, "Created ext. priority queue, capacity: %llu elems\n", q.capacity());

    LOG(V2_INFO, "Adding elements ...\n");

    auto numElems = 268435456;
    for (unsigned long x = 1; x <= numElems; x++) {
        q.push(x);
    }

    LOG(V2_INFO, "Added elements\n");
    LOG(V2_INFO, "Removing elements ...\n");

    for (unsigned long x = numElems; x > 0; x--) {
        assert(!q.empty());
        assert(q.top() == x);
        q.pop();
    }
    assert(q.empty());

    LOG(V2_INFO, "Removed elements\n");
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    Process::init(0);
    ProcessWideThreadPool::init(1);

    testBasic();
}


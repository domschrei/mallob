
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

#include "app/sat/util/reverse_lrat_parser.hpp"
#include "util/external_priority_queue.hpp"


void testWithPriorityQueue(const std::string& filename) {

    ExternalPriorityQueue<unsigned long> pq;
    int numPushed = 0;
    int numUntraced = 0;
    int numSkipped = 0;

    auto time = Timer::elapsedSeconds();
    ReverseLratParser parser(filename);
    while (parser.hasNext()) {
        
        auto line = parser.next();
        
        while (!pq.empty() && pq.top() > line.id) {
            LOG(V1_WARN, "[WARN] ID %ld untraced!\n", pq.top());
            pq.pop();
            numUntraced++;
        }

        bool keep = line.literals.empty() || !pq.empty();
        if (!pq.empty()) {
            assert(pq.top() == line.id);
            while (pq.top() == line.id) pq.pop();
        }

        std::string lits;
        for (int lit : line.literals) lits += std::to_string(lit) + " ";
        std::string hints;
        for (int hint : line.hints) hints += std::to_string(hint) + " ";
        LOG(V2_INFO, "id=%ld lits=(%s) hints=(%s)\n", line.id, lits.c_str(), hints.c_str());

        if (keep) {
            for (auto hint : line.hints) {
                numPushed++;
                pq.push(std::abs(hint));
            }
        } else {
            numSkipped++;
        }
    }
    time = Timer::elapsedSeconds() - time;

    auto numReadLines = parser.getNumReadLines();
    LOG(V2_INFO, "Read %ld lines (%i pushed, %i untraced, %i skipped) in %.5fs (%.2f l/s)\n", 
        numReadLines, numPushed, numUntraced, numSkipped, time, numReadLines/time);
    assert(numUntraced == 0);
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    Process::init(0);
    ProcessWideThreadPool::init(1);

    auto files = {
        "instances/proofs/example-4-vars.lrat", 
        "instances/proofs/example-5-vars.lrat", 
        "instances/proofs/uuf-30-1.lrat", 
        "instances/proofs/uuf-50-2.lrat", 
        "instances/proofs/uuf-50-3.lrat", 
        "instances/proofs/uuf-100-1.lrat", 
        "instances/proofs/uuf-100-2.lrat", 
        "instances/proofs/uuf-100-3.lrat", 
        "instances/proofs/uuf-100-4.lrat", 
        "instances/proofs/uuf-100-5.lrat"
    };
    for (auto& file : files) testWithPriorityQueue(file);
}


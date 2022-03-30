
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>
#include <thread>
#include <set>
#include <random>
#include <unistd.h>
#include <algorithm>


#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/terminator.hpp"
#include "app/sat/job/distributed_clause_filter.hpp"


int main() {
    
    int seed = 1;

    Timer::init();
    Random::init(seed, seed);
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    Process::init(0);
    ProcessWideThreadPool::init(1);

    int numClauses = 100'000;

    std::vector<std::vector<int>*> clauseData;
    for (int c = 0; c < numClauses; c++) {
        std::vector<int>* data = new std::vector<int>();
        for (int i = 0; i < 1+Random::rand()*29; i++) 
            data->push_back((int) (10000 + 90000*Random::rand()));
        clauseData.push_back(data);
    }
    std::vector<Clause> clauses;
    for (auto data : clauseData) {
        clauses.push_back(Clause(data->data(), data->size(), 2+Random::rand()*(data->size()-2)));
    }
    sort(clauses.begin(), clauses.end(), 
    [](const Clause& a, const Clause& b) -> bool
    { 
        return ClauseHasher::hash(a, /*which=*/3) < ClauseHasher::hash(b, /*which=*/3); 
    });

    for (int volume = 1; volume <= 32; volume++) {

        std::vector<std::vector<bool>> passedMatrix;

        int allNumFiltered = 0;
        for (int rank = 0; rank < volume; rank++) {
            DistributedClauseFilter filter(/*filterMemory=*/60);
            filter.update(rank, volume);
            std::vector<bool> r;
            int numFiltered = 0;
            for (auto& c : clauses) {
                filter.passClause(c, /*epoch=*/1);
                r.push_back(filter.passClause(c, /*epoch=*/2));
                if (!r.back()) numFiltered++;
            }
            passedMatrix.push_back(std::move(r));
            auto indices = filter.getSlotIndices();
            LOG(V2_INFO, "[v=%i] Rank %i : indices [%i,%i,%i], %i filtered\n", volume, rank, indices[0], indices[1], indices[2], numFiltered);
            allNumFiltered += numFiltered;
        }
        LOG(V2_INFO, "[v=%i] Total filtered: %i\n\n", volume, allNumFiltered);
    }


    
    //for (auto& c : clauses) LOG(V2_INFO, "%s ~> %lu\n", c.toStr().c_str(), ClauseHasher::hash(c, /*which=*/3));
    //for (auto& v : passedMatrix) {
    //    std::string out;
    //    for (bool passed : v) out += (passed ? " " : "X");
    //    LOG(V2_INFO, "%s\n", out.c_str());
    //}
}


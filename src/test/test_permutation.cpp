
#include <assert.h>
#include <set>

#include "util/random.hpp"
#include "util/permutation.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

void testPermutations() {

    std::vector<int> ns({8, 10, 12, 14, 16, 18, 20, 30, 32, 34, 48, 64, 128, 256, 512, 1024});
    std::vector<int> rs({1, 2, 3, 4, 5, 8, 16});

    for (int r : rs) {
        for (int n : ns) {
            if (n < 2*r) continue;

            log(V2_INFO, "n=%i, r=%i\n", n, r);
            for (int rank = 0; rank < n; rank++) {
                log(V2_INFO, " rank=%i\n", rank);
                auto permutations = AdjustablePermutation::getPermutations(n, r);
                std::vector<int> outgoingEdges = AdjustablePermutation::createExpanderGraph(permutations, rank);
                
                // Correctness checks
                assert(outgoingEdges.size() == r);
                std::set<int> seenEdges;
                for (int edge : outgoingEdges) {
                    //printf("  %i\n", edge);
                    assert(edge != rank);
                    assert(!seenEdges.count(edge));
                    seenEdges.insert(edge);
                }
            }
        }
    }
}

void testBestOutgoingEdges() {

    int r = 4;
    int n = 1000;
    log(V2_INFO, "n=%i, r=%i\n", n, r);
    auto permutations = AdjustablePermutation::getPermutations(n, r);
    int root = 1; 
    std::vector<int> occ(n, 0);
    for (size_t i = 0; i < n; i++) {
        log(V2_INFO, " rank=%i\n", i);
        auto edges = AdjustablePermutation::getBestOutgoingEdgeForEachNode(permutations, i);
        occ[edges[root]]++;
    }
    std::string out = "";
    for (int x : occ) out += std::to_string(x) + " ";
    log(V2_INFO, " occ: %s\n", out.c_str());
}

int main() {

    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG, false, false, false, nullptr);

    testBestOutgoingEdges();
    testPermutations();

    return 0;
}
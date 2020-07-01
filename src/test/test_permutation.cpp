
#include <assert.h>
#include <set>

#include "util/random.hpp"
#include "util/permutation.hpp"
#include "util/console.hpp"
#include "util/sys/timer.hpp"

/*
Compile with:
g++ -g -Isrc src/test/test_permutation.cpp src/util/{random,console,permutation,timer,params}.cpp -o test_permutation
*/

int main() {

    Timer::init();
    Random::init(rand(), rand());
    Console::init(0, Console::VVVVERB, false, false, false, "/dev/null");

    std::vector<int> ns({8, 10, 12, 14, 16, 18, 20, 30, 32, 34, 48, 64, 128, 256, 512, 1024});
    std::vector<int> rs({1, 2, 3, 4, 5, 8, 16});

    for (int r : rs) {
        for (int n : ns) {
            if (n < 2*r) continue;

            Console::log(Console::INFO, "n=%i, r=%i", n, r);
            for (int rank = 0; rank < n; rank++) {
                Console::log(Console::INFO, " rank=%i", rank);
                std::vector<int> outgoingEdges = AdjustablePermutation::createExpanderGraph(n, r, rank);
                
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

    return 0;
}
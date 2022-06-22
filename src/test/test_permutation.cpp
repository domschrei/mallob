
#include "util/assert.hpp"
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

            LOG(V2_INFO, "n=%i, r=%i\n", n, r);
            for (int rank = 0; rank < n; rank++) {
                LOG(V2_INFO, " rank=%i\n", rank);
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
    LOG(V2_INFO, "n=%i, r=%i\n", n, r);
    auto permutations = AdjustablePermutation::getPermutations(n, r);
    std::vector<std::vector<int>> allEdges;
    for (size_t i = 0; i < n; i++) {
        LOG(V2_INFO, " rank=%i\n", i);
        auto edges = AdjustablePermutation::getBestOutgoingEdgeForEachNode(permutations, i);
        allEdges.push_back(std::move(edges));
    }

    std::vector<int> nodesPerLevel(100, 0);
    for (int root = 0; root < n; root++) {
        
        robin_hood::unordered_map<int, std::vector<int>> graph;
        robin_hood::unordered_map<int, int> depth;
        for (int rank = 0; rank < n; rank++) {
            int src = allEdges[rank][root];
            int dest = rank;
            if (src != dest) graph[src].push_back(dest);
        }
        std::vector<int> nodeStack;
        nodeStack.push_back(root);
        int level = 0;
        int visited = 0;
        while (!nodeStack.empty()) {
            std::vector<int> newNodeStack;
            LOG(V2_INFO, "%i nodes on level %i\n", nodeStack.size(), level);
            nodesPerLevel[level] += nodeStack.size();
            for (int node : nodeStack) {
                visited++;
                for (int succ : graph[node]) {
                    newNodeStack.push_back(succ);
                }
            }
            nodeStack = newNodeStack;
            level++;
        }
        LOG(V2_INFO, "root=%i : %i visited\n", root, visited);

        /*
        for (const auto& [src, dests] : graph) {
            if (dests.empty()) continue;
            std::string str;
            for (int d : dests) str += std::to_string(d) + " ";
            str = str.substr(0, str.size()-1);
            LOG(V2_INFO, "%i -> {%s}\n", src, str.c_str());
        }*/
    }

    for (size_t i = 0; i < nodesPerLevel.size(); i++) {
        LOG(V2_INFO, "l=%i n=%.3f\n", i, nodesPerLevel[i] / (float)n);
    }
}

int main() {

    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);

    testBestOutgoingEdges();
    testPermutations();
}
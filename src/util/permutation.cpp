
#include "util/assert.hpp"
#include <set>

#include "permutation.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"

std::vector<std::vector<int>> AdjustablePermutation::getPermutations(int n, int degree) {

    std::vector<AdjustablePermutation*> permutations;

    // First outgoing edges:
    // form a cycle of all nodes through some first permutation
    // (prevents possibility of disconnected graph)
    // => 2·n permutation queries, O(n) local work
    // => O(n) space
    std::vector<int> intrinsicPartners(n);
    {
        AdjustablePermutation pInit(n, /*seed=*/n-1);
        for (int pos = 0; pos < n; pos++) {
            int val = pInit.get(pos);
            int nextVal = pInit.get((pos+1) % n);
            intrinsicPartners[val] = nextVal;
        }
    }

    // Blackbox checker whether some value at some position of a new permutation
    // is valid w.r.t. previous permutations
    // => r permutation queries, O(r) local work
    auto isValid = [&permutations, &intrinsicPartners](int pos, int val) {
        if (pos == val) return false; // no identity!
        if (intrinsicPartners[pos] == val) return false; // not the intrinsic partner!
        for (auto& perm : permutations) {
            if (perm->get(pos) == val) return false;
        }
        return true;
    };

    // Do r-1 times (as the first edge is already identified)
    for (int r = 1; r < degree; r++) {

        // Generate global permutation over all worker ranks
        // disallowing identity and previous permutations
        AdjustablePermutation* p = new AdjustablePermutation(n, n*(r+1));
        
        // For each position of the permutation, left to right
        for (int pos = 0; pos < n; pos++) {
            int val = p->get(pos);
            if (!isValid(pos, val)) {
                // Value at this position is not valid

                // Find a position of this permutation to swap values
                int swapPos;
                int swapVal;
                bool successfulSwap = false;
                while (!successfulSwap) {
                    // Draw a random swap position that is NOT the current position,
                    // get the according swap value
                    swapPos = (int) (Random::global_rand()*(n-1));
                    if (swapPos >= pos) swapPos++;
                    swapVal = p->get(swapPos);

                    // Check if it can be swapped:
                    // swap value is valid at current position
                    // AND current value is valid at swap position
                    if (isValid(pos, swapVal) && isValid(swapPos, val))
                        successfulSwap = true;
                }

                // Adjust permutation
                p->adjust(pos, swapVal);
                p->adjust(swapPos, val);
            }
        }
        
        permutations.push_back(p);
    }

    // Transform output and clean up
    std::vector<std::vector<int>> out;
    out.push_back(std::move(intrinsicPartners));
    for (size_t i = 0; i < permutations.size(); i++) {
        std::vector<int> thisPerm(n);
        for (size_t j = 0; j < n; j++) thisPerm[j] = permutations[i]->get(j);
        out.push_back(std::move(thisPerm));
        delete permutations[i];
    }

    return out;
}

std::vector<int> AdjustablePermutation::createUndirectedExpanderGraph(int n, int degree, int myRank) {
    assert(degree % 2 == 0);
    
    std::vector<int> myEdges;

    // Get index of this worker within a particular permutation of all ranks
    AdjustablePermutation p(n, /*seed=*/(n-1)*degree);
    int myIndex = 0;
    while (p.get(myIndex) != myRank) myIndex++;

    // Add edges to workers which have a specific distance to this worker
    // with respect to the drawn permutation of ranks
    for (size_t i = 0; i < degree/2; i++) {
        int offset = 1 + i * std::max(1, (n / (degree-1)));
        myEdges.push_back(p.get((n+myIndex+offset) % n));
        myEdges.push_back(p.get((n+myIndex-offset) % n));
    }

    return myEdges;
}

std::vector<int> AdjustablePermutation::createExpanderGraph(const std::vector<std::vector<int>>& permutations, int myRank) {

    if (myRank == 0) {
        for (size_t i = 0; i < permutations.size(); i++) {
            std::string str;
            for (size_t j = 0; j < permutations[i].size(); j++) str += std::to_string(permutations[i][j]) + " ";
            LOG(V5_DEBG, "Perm. %i: %s\n", i, str.c_str());
        }
    }

    std::vector<int> outgoingEdges(permutations.size());
    for (size_t i = 0; i < permutations.size(); i++) outgoingEdges[i] = permutations[i][myRank];
    return outgoingEdges;
}

/*
Perform a modified Dijkstra's algorithm to find the outgoing edge from PE #myRank 
along the shortest path towards each PE. For each PE x, this constructs a distributed 
r-ary reduction tree of PEs that is probabilistically balanced and rooted at x.
*/
std::vector<int> AdjustablePermutation::getBestOutgoingEdgeForEachNode(const std::vector<std::vector<int>>& permutations, int myRank) {
    
    size_t numNodes = permutations[0].size();
    size_t degree = permutations.size();

    std::vector<int> distance(numNodes, INT32_MAX);
    distance[myRank] = 0;
    
    auto comp = [&distance, myRank](int x, int y) {
        if (distance[x] != distance[y]) return distance[x] < distance[y];
        // Tie breaking is done pseudo-randomly and differently for each PE:
        // Helps to distribute the child nodes more uniformly
        return robin_hood::hash_int(x+myRank) < robin_hood::hash_int(y+myRank);
    };
    auto unvisitedSet = std::set<int, decltype(comp)>(comp);
    for (size_t i = 0; i < numNodes; i++) unvisitedSet.insert(i);

    std::vector<int> bestOutgoingEdge(numNodes, -1);

    while (!unvisitedSet.empty()) {
        int currentNode = *unvisitedSet.begin();
        
        // For each of the current node's successors:
        for (size_t i = 0; i < permutations.size(); i++) {
            int succNode = permutations[i][currentNode];
            if (!unvisitedSet.count(succNode)) continue;

            // Initialize best outgoing edge for each of the initial node's neighbors
            if (currentNode == myRank) {
                bestOutgoingEdge[currentNode] = succNode; 
            }
            assert(bestOutgoingEdge[currentNode] >= 0);

            // Update distance and best outgoing edge
            int newDistance = distance[currentNode] + 1;
            if (newDistance < distance[succNode]) {
                unvisitedSet.erase(succNode);
                distance[succNode] = newDistance;
                unvisitedSet.insert(succNode);
                bestOutgoingEdge[succNode] = bestOutgoingEdge[currentNode];
            }
        }

        unvisitedSet.erase(currentNode);
    }
    bestOutgoingEdge[myRank] = myRank;

    /*
    std::string str = "";
    for (size_t i = 0; i < bestOutgoingEdge.size(); i++) {
        str += std::to_string(i) + std::string("->") + std::to_string(bestOutgoingEdge[i]) + std::string(" ");
    }
    LOG(V4_VVER, "Best out edges: %s\n", str.c_str());
    */

    return bestOutgoingEdge;
}



AdjustablePermutation::AdjustablePermutation(int n, int seed) {

    _n = n;
    _rng = std::mt19937(seed);

    // Create explicit permutations [0, sqrt(n)) -> [0, sqrt(n))
    int feistelRounds = 3;
    _root_n = std::ceil(sqrt((float) n));
    _feistels.reserve(feistelRounds);
    for (int f = 1; f <= feistelRounds; f++) {
        _feistels.push_back(std::vector<int>());
        std::vector<int>& feistel = _feistels.back();

        // Generate and shuffle number sequence
        feistel.reserve(_root_n);
        for (int i = 0; i < _root_n; i++) feistel.push_back(i);
        std::shuffle(std::begin(feistel), std::end(feistel), _rng);
    }
}

int AdjustablePermutation::get(int x, bool checkAdjusted) const {
    if (x < 0 || x >= _n) {
        LOG(V1_WARN, "[WARN] Invalid input for adj.perm. [0,%i) : %i\n", _n, x);
        while (x < 0) x += 100*_n;
        x = x % _n;
    }

    if (checkAdjusted && _adjusted_values.count(x)) {
        return _adjusted_values.at(x);
    }

    int input = x;
    int k = 0;

    while (true) {
        // Conversion
        // x = b · n + a
        int b = x / _root_n;
        int a = x % _root_n;

        // Feistel rounds
        for (unsigned int i = 0; i < _feistels.size(); i++) {
            int out_a = b;
            int out_b = (a + _feistels[i][b]) % _root_n;
            a = out_a;
            b = out_b;
        }

        // Back-conversion
        x = a + _root_n * b;

        k++;

        // Prerequisite 1: must be in valid domain [0, n)
        if (x >= _n) continue;
        // Prerequisite 2: must not map to identity
        if (_identity_disallowed && x == input) continue;
        // Prerequisite 3: must not map to the according value 
        // of some of the disallowed permutations
        bool accept = true;
        for (const auto& p : _disallowed_permutations) {
            if (x == p->get(input)) {
                accept = false;
                break;
            }
        }
        if (accept) break;
    }

    return x;
}

void AdjustablePermutation::adjust(int x, int new_x) {
    if (x < 0 || x >= _n) {
        LOG(V1_WARN, "[WARN] Invalid input for adj.perm. [0,%i) : %i\n", _n, x);
        while (x < 0) x += 100*_n;
        x = x % _n;
    }
    if (get(x) != new_x) _adjusted_values[x] = new_x;
}

void AdjustablePermutation::clear(int x) {
    _adjusted_values.erase(x);
}

void AdjustablePermutation::clear() {
    _adjusted_values.clear();
}

void AdjustablePermutation::setIdentityDisallowed(const bool& disallow) {
    _identity_disallowed = disallow;
}

void AdjustablePermutation::addDisallowedPermutation(AdjustablePermutation* p) {
    _disallowed_permutations.push_back(p);
}
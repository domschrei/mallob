
#include <assert.h>

#include "permutation.hpp"
#include "util/console.hpp"
#include "util/random.hpp"

std::vector<int> AdjustablePermutation::createExpanderGraph(int n, int degree, int myRank) {

    std::vector<int> outgoingEdges;
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
            //Console::log(Console::INFO, "%i => %i", val, nextVal);
        }
        outgoingEdges.push_back(intrinsicPartners[myRank]);
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
        
        if (myRank == 0) {
            Console::append(Console::VVERB, "Permutation %i  : ", r);
            for (int pos = 0; pos < n; pos++) {
                Console::append(Console::VVERB, "%i ", p->get(pos));
            }
            Console::log(Console::VVERB, "");
        }

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
                if (myRank == 0) Console::log(Console::VVERB, "SWAP %i@%i <-> %i@%i", swapVal, swapPos, val, pos);
            }
        }
        
        //if (myRank == 0) {
            Console::append(Console::VERB, "Permutation %i' : ", r);
            for (int pos = 0; pos < n; pos++) {
                Console::append(Console::VERB, "%i ", p->get(pos));
            }
            Console::log(Console::VERB, "");
        //}

        // Check that the amount of incoming edges to this node is correct
        int numIncoming = 0;
        for (int pos = 0; pos < n; pos++) {
            if (p->get(pos) == myRank) numIncoming++;
        }
        assert(numIncoming == 1 || Console::fail("Rank %i : %i incoming edges!", myRank, numIncoming));

        permutations.push_back(p);
        outgoingEdges.push_back(p->get(myRank));
    }

    // Clean up
    for (size_t i = 0; i < permutations.size(); i++) {
        delete permutations[i];
    }

    return outgoingEdges;
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

int AdjustablePermutation::get(int x) const {
    if (x < 0 || x >= _n) {
        Console::log(Console::WARN, "Invalid input for adj.perm. [0,%i) : %i", _n, x);
        while (x < 0) x += 100*_n;
        x = x % _n;
    }

    if (_adjusted_values.count(x)) {
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
        Console::log(Console::WARN, "Invalid input for adj.perm. [0,%i) : %i", _n, x);
        while (x < 0) x += 100*_n;
        x = x % _n;
    }
    if (get(x) != new_x) _adjusted_values[x] = new_x;
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
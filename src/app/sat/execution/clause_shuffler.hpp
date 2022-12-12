
#pragma once

#include "util/random.hpp"

class ClauseShuffler {

private:
    std::vector<int> _input;
    std::vector<std::pair<size_t, int*>> _clauses;

    std::mt19937 _rng;
    std::uniform_real_distribution<float> _dist;
    std::function<float()> _rng_func;

    size_t _it;

public:
    ClauseShuffler(int seed = 0) : _rng(std::mt19937(seed)), _dist(std::uniform_real_distribution<float>(0, 1)) {
        _rng_func = [this]() {return _dist(_rng);};
    }

    std::pair<const int*, size_t> doShuffle(const int* input, size_t inputSize, bool permuteClauses = true, bool permuteLiterals = true) {

        _input = std::vector<int>(input, input+inputSize);

        // Build vector of clauses (size + pointer to data)
        _clauses.clear();
        size_t clauseStart = 0; // 1st clause always begins at position 0
        size_t sumOfSizes = 0;
        for (size_t i = 0; i < inputSize; i++) { // for each literal
            if (_input[i] == 0) {
                // clause ends
                size_t thisClauseSize = i-clauseStart;
                _clauses.emplace_back(thisClauseSize, _input.data()+clauseStart);
                clauseStart = i+1; // next clause begins at subsequent position
                sumOfSizes += thisClauseSize;
            }
        }
        assert(sumOfSizes + _clauses.size() == inputSize);

        if (permuteClauses) {
            // Shuffle order of clauses
            random_shuffle(_clauses.data(), _clauses.size(), _rng_func);
        }
        if (permuteLiterals) {
            // Shuffle order of literals within each clause
            for (auto& [size, data] : _clauses) random_shuffle(data, size, _rng_func);
        }

        return std::pair<const int*, size_t>(_input.data(), _input.size());
    }
};

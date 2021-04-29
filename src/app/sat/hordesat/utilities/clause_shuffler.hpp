
#ifndef DOMPASCH_MALLOB_CLAUSE_SHUFFLER_HPP
#define DOMPASCH_MALLOB_CLAUSE_SHUFFLER_HPP

#include "util/shuffle.hpp"

class ClauseShuffler {

private:
    size_t _input_size;
    const int* _input;
    bool _permute_literals;

    std::mt19937 _rng;
    std::uniform_real_distribution<float> _dist;
    std::function<float()> _rng_func;

    std::vector<std::pair<size_t, const int*>> _clauses;
    size_t _it;

public:
    ClauseShuffler(size_t size, const int* input, int seed = 0) : _input_size(size), _input(input),
        _rng(std::mt19937(seed)), _dist(std::uniform_real_distribution<float>(0, 1)) {
            _rng_func = [this]() {return _dist(_rng);};
        }
    
    void doShuffle(bool permuteClauses = true, bool permuteLiterals = true) {

        // Build vector of clauses (size + pointer to data)
        _clauses.clear();
        size_t clauseStart = 0; // 1st clause always begins at position 0
        size_t sumOfSizes = 0;
        for (size_t i = 0; i < _input_size; i++) { // for each literal
            if (_input[i] == 0) {
                // clause ends
                size_t thisClauseSize = i-clauseStart;
                _clauses.emplace_back(thisClauseSize, _input+clauseStart);
                clauseStart = i+1; // next clause begins at subsequent position
                sumOfSizes += thisClauseSize;
            }
        }
        assert(sumOfSizes + _clauses.size() == _input_size);
        
        if (permuteClauses) {
            // Shuffle order of clauses
            shuffle(_clauses.data(), _clauses.size(), _rng_func);
        }

        _it = 0;
        _permute_literals = permuteLiterals;
    }

    bool hasNextClause() {
        return _it < _clauses.size();
    }
    std::vector<int> nextClause() {
        auto [size, data] = _clauses[_it++];
        std::vector<int> cls(data, data+size+1); // with separator zero
        if (_permute_literals) {
            // Shuffle order of literals within clause
            shuffle(cls.data(), size, _rng_func); // do not shuffle separator zero
        }
        return cls;
    }
};

#endif
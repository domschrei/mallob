
#pragma once

#include "util/shuffle.hpp"
#include "util/logger.hpp"

class SerializedFormulaParser {

private:
    const size_t _size;
    const int* _payload;

    bool _shuffle_clauses {false};
    bool _shuffle_literals {false};

    std::mt19937 _rng;
    std::uniform_real_distribution<float> _dist;
    std::function<float()> _rng_func;

    // Iterating over non-shuffled clauses
    size_t _index {0};

    // Iterating over shuffled clauses
    std::vector<std::pair<int, const int*>> _shuffled_clauses;
    int _clause_index {0};

    // Iterating over shuffled literals
    std::vector<int> _current_clause;
    int _lit_index_in_clause {0};

    bool _output_final_zero {false};


public:
    SerializedFormulaParser(size_t size, const int* literals) : _size(size), _payload(literals) {}

    void shuffle(int seed, bool shuffleClauses, bool shuffleLiterals) {
        _shuffle_clauses = shuffleClauses;
        _shuffle_literals = shuffleLiterals;
        _rng = std::mt19937(seed);
        _dist = std::uniform_real_distribution<float>(0, 1);
        _rng_func = [this]() {return _dist(_rng);};

        if (_shuffle_clauses) performShuffle();
    }

    bool getNextLiteral(int& lit) {

        // Plain iteration over the payload
        if (!_shuffle_clauses && !_shuffle_literals) {
            if (_index == _size) return false;
            lit = _payload[_index];
            _index++;
            return true;
        }

        int size;
        const int* lits;

        // Fetch current clause
        if (_shuffle_literals) {
            // Read copied and shuffled clause
            if (_lit_index_in_clause == _current_clause.size()) {
                
                bool firstClause = _current_clause.empty();

                // "Refill" next clause
                if (_shuffle_clauses) {
                    if (_clause_index == _shuffled_clauses.size()) return outputFinalZero(lit);
                    auto [clsSize, clsLits] = _shuffled_clauses[_clause_index];
                    _current_clause.resize(clsSize);
                    for (int i = 0; i < clsSize; i++) {
                        _current_clause[i] = clsLits[i];
                    }
                    _clause_index++;
                } else {
                    if (_index == _size) return outputFinalZero(lit);
                    int startIdx = _index;
                    int endIdx = startIdx;
                    while (_payload[endIdx] != 0) endIdx++;
                    _current_clause.resize(endIdx-startIdx);
                    for (int i = 0; i < endIdx-startIdx; i++) {
                        _current_clause[i] = _payload[startIdx+i];
                    }
                    _index = endIdx+1;
                }
                ::shuffle(_current_clause.data(), _current_clause.size(), _rng_func);
                _lit_index_in_clause = 0;

                if (!firstClause) {
                    // emit separator zero
                    lit = 0;
                    return true;
                }
            }
            size = _current_clause.size();
            lits = _current_clause.data();
        } else {
            // Read clause directly from shuffled sequence of clauses
            if (_clause_index == _shuffled_clauses.size()) return false;
            auto pair = _shuffled_clauses[_clause_index];
            size = pair.first;
            lits = pair.second;
            if (_lit_index_in_clause == size) {
                _clause_index++;
                _lit_index_in_clause = 0;
                
                // emit separator zero
                lit = 0;
                return true;
            }
        }

        // Fetch the clause's current literal
        lit = lits[_lit_index_in_clause];
        // Go to next literal / clause
        _lit_index_in_clause++;
        return true;
    }

    size_t getPayloadSize() const {
        return _size;
    }

private:

    bool outputFinalZero(int& lit) {
        if (!_output_final_zero) {
            _output_final_zero = true;
            lit = 0;
            return true;
        } else return false;
    }

    void performShuffle() {

        // Build vector of clauses (size + pointer to data)
        size_t clauseStart = 0; // 1st clause always begins at position 0
        size_t sumOfSizes = 0;
        for (size_t i = 0; i < _size; i++) { // for each literal
            if (_payload[i] == 0) {
                // clause ends
                size_t thisClauseSize = i-clauseStart;
                _shuffled_clauses.emplace_back(thisClauseSize, _payload+clauseStart);
                clauseStart = i+1; // next clause begins at subsequent position
                sumOfSizes += thisClauseSize;
            }
        }
        assert(sumOfSizes + _shuffled_clauses.size() == _size);

        // Shuffle order of clauses
        ::shuffle(_shuffled_clauses.data(), _shuffled_clauses.size(), _rng_func);
    }
};

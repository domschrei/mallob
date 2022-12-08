
#pragma once

#include "util/shuffle.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/permutation.hpp"

class SerializedFormulaParser {

private:
    Logger& _logger;

    const size_t _size;
    const int* _payload;

    bool _shuffled {false};

    AdjustablePermutation _permutation;
    std::vector<const int*> _clause_refs;

    int _clause_index {0};
    int _permuted_clause_index {-1};

    const int* _literal_ptr {nullptr};
    const int* _next_literal_ptr {nullptr};

public:
    SerializedFormulaParser(Logger& logger, size_t size, const int* literals) : 
        _logger(logger), _size(size), _payload(literals), 
        _literal_ptr(_size==0 ? nullptr : _payload), _next_literal_ptr(_payload+_size) {}

    void shuffle(int seed) {
        
        auto time = Timer::elapsedSeconds();

        // Build vector of clauses (size + pointer to data)
        size_t clauseStart = 0; // 1st clause always begins at position 0
        size_t sumOfSizes = 0;
        for (size_t i = 0; i < _size; i++) { // for each literal
            if (_payload[i] == 0) {
                // clause ends
                size_t thisClauseSize = i-clauseStart;
                _clause_refs.push_back(_payload+clauseStart);
                //_shuffled_clauses.emplace_back(thisClauseSize, _payload+clauseStart);
                clauseStart = i+1; // next clause begins at subsequent position
                sumOfSizes += thisClauseSize;
            }
        }
        assert(sumOfSizes + _clause_refs.size() == _size);

        if (_clause_refs.size() > 128) {
            // Reduce the set of clause references for better performance
            // -> shuffles blocks of clauses instead of individual clauses
            for (size_t i = 0; i < 128; i++) {
                float floatIndex = (i/128.0) * _clause_refs.size();
                int index = std::min(_clause_refs.size()-1, (size_t)std::round(floatIndex));
                auto ref = _clause_refs[index];
                _clause_refs[i] = ref;
            }
            _clause_refs.resize(128);
        }

        // Generate permutation of clauses
        _permutation = AdjustablePermutation(_clause_refs.size(), seed);

        time = Timer::elapsedSeconds() - time;
        LOGGER(_logger, V4_VVER, "Shuffling took %.4fs\n", time);
        
        _shuffled = true;
        _literal_ptr = nullptr;
        _next_literal_ptr = nullptr;
    }

    bool getNextLiteral(int& lit) {

        // No valid current clause?
        if (!_literal_ptr) {
            // Pick next clause
            if (_clause_index == _clause_refs.size()) {
                // All clauses read
                return false;
            }
            // Draw permuted clause index
            _permuted_clause_index = _permutation.get(_clause_index, false);
            // Set pointers to the current clause and its end
            _literal_ptr = _clause_refs[_permuted_clause_index];
            _next_literal_ptr = _permuted_clause_index+1 == _clause_refs.size() ? 
                _payload+_size
                : _clause_refs[_permuted_clause_index+1];
            // Advance clause counter
            ++_clause_index;
        }

        // Set literal to destination of the current pointer
        lit = *_literal_ptr;

        // Advance literal pointer
        ++_literal_ptr;
        if (_literal_ptr == _next_literal_ptr) {
            // Clause fully read -- pick next clause next time
            _literal_ptr = nullptr;
        }

        return true; // success
    }

    size_t getPayloadSize() const {
        return _size;
    }
};


#pragma once

#include <functional>

#include "../../data/clause.hpp"
#include "buffer_iterator.hpp"

class BufferReducer {

private:
    int* _buffer;
    int _size;
    int _max_clause_length;
    bool _slots_for_sum_of_length_and_lbd;

public:
    BufferReducer(int* buffer, int size, int maxClauseLength, bool slotsForSumOfLengthAndLbd) : 
        _buffer(buffer), _size(size), _max_clause_length(maxClauseLength), 
        _slots_for_sum_of_length_and_lbd(slotsForSumOfLengthAndLbd) {}

    int reduce(std::function<bool()> acceptor) {
        return reduceTemplated(acceptor);
    }
    int reduce(std::function<bool(const Mallob::Clause&)> acceptor) {
        return reduceTemplated(acceptor);
    }

private:
    template <typename Func>
    int reduceTemplated(Func acceptor) {

        const int numInts = sizeof(size_t)/sizeof(int);
        if (_size <= numInts) return _size;

        size_t currentPos = numInts+1;
        size_t currentWritePos = currentPos;

        BufferIterator it(_max_clause_length, _slots_for_sum_of_length_and_lbd);
        Mallob::Clause currentClause(nullptr, it.clauseLength, it.lbd);
        
        // Local variable we count down on
        int remainingClsOfBucket = _buffer[numInts];
        // Reference we modify only if clauses are filtered
        int* clsInBucketCounter = &_buffer[numInts];
        assert(remainingClsOfBucket >= 0);

        while (true) {
            // Find a non-empty bucket
            if (remainingClsOfBucket == 0) {
                do {    
                    // Nothing left to read?
                    if (currentPos >= _size) return currentWritePos;

                    // Go to next bucket
                    it.nextLengthLbdGroup();
                    _buffer[currentWritePos] = _buffer[currentPos]; // copy to new location
                    remainingClsOfBucket = _buffer[currentWritePos];
                    clsInBucketCounter = &_buffer[currentWritePos];
                    currentPos++;
                    currentWritePos++;
                    assert(remainingClsOfBucket >= 0);
                
                } while (remainingClsOfBucket == 0);

                // Update clause data
                if constexpr (std::is_same<std::function<bool(const Mallob::Clause&)>, Func>::value) {
                    currentClause.size = it.clauseLength;
                    currentClause.lbd = it.lbd;
                }
            }

            // Does clause exceed bounds of the buffer?
            if (currentPos+it.clauseLength > _size) return currentWritePos;

            // Test acceptance
            bool accepted;
            if constexpr (std::is_same<std::function<bool(const Mallob::Clause&)>, Func>::value) {
                // Set pointer to literals
                assert(_buffer[currentPos] != 0);
                currentClause.begin = _buffer+currentPos;
                // currentClause is a valid clause now
                accepted = acceptor(currentClause);
            } else {
                accepted = acceptor();
            }

            if (accepted) {
                // Clause accepted: Re-position clause, advance both read and write positions
                for (size_t i = 0; i < it.clauseLength; i++) {
                    _buffer[currentWritePos++] = _buffer[currentPos++];
                }
            } else {
                // Clause filtered: Only advance read position
                currentPos += it.clauseLength;
                (*clsInBucketCounter)--; // decrement nb. of clauses in this bucket
            }

            // Decrement remaining clauses
            remainingClsOfBucket--;
        }

        return currentWritePos;
    }
};

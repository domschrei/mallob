
#pragma once

#include "app/sat/data/clause_metadata.hpp"
#include "util/assert.hpp"

struct BufferIterator {

int maxEffClauseLength;
bool slotsForSumOfLengthAndLbd;
int maxSumOfLengthAndLbd;

int clauseLength;
int lbd;

BufferIterator() : maxEffClauseLength(0), slotsForSumOfLengthAndLbd(false), maxSumOfLengthAndLbd(0) {}
BufferIterator(int maxEffClauseLength, bool slotsForSumOfLengthAndLbd) :
    maxEffClauseLength(maxEffClauseLength), slotsForSumOfLengthAndLbd(slotsForSumOfLengthAndLbd), 
    maxSumOfLengthAndLbd(maxEffClauseLength+2) {

    clauseLength = 1;
    lbd = 1;
}
BufferIterator(const BufferIterator& other) : maxEffClauseLength(other.maxEffClauseLength), 
    slotsForSumOfLengthAndLbd(other.slotsForSumOfLengthAndLbd), 
    maxSumOfLengthAndLbd(other.maxSumOfLengthAndLbd), clauseLength(other.clauseLength), lbd(other.lbd) {}

void reset() {
    clauseLength = 1;
    lbd = 1;
}

bool storeWithExplicitLbd(int maxLbdPartitionedSize) const {
    
    if (slotsForSumOfLengthAndLbd && (clauseLength+lbd) >= 6 
            && (clauseLength+lbd) <= maxSumOfLengthAndLbd) {
        // Shared slot for all clauses of this length+lbd sum
        return true;
    } else if (clauseLength > maxLbdPartitionedSize) {
        // Slot for all clauses of this length
        return true;
    } else {
        // Exclusive slot for this length-lbd combination
        return false;
    }
}

void nextLengthLbdGroup() {

    if (slotsForSumOfLengthAndLbd && clauseLength+lbd <= maxSumOfLengthAndLbd) {

        // Beginning: Clauses with LBD <= 2
        if (clauseLength < 3 && lbd <= 2) {
            clauseLength++;
            if (clauseLength == 2) lbd++; // only unit clauses have LBD 1
            assert(lbd == 2);
            return;
        }
        
        // Jump to the top right of the next diagonal
        // (which could bring the sum of length+lbd over the threshold)
        if (lbd == 2) {
            clauseLength++;
            while (lbd+1 <= clauseLength-1) {
                lbd++;
                clauseLength--;
            }
            return;
        }

        // Go diagonally to the bottom left until LBD=2
        clauseLength++;
        lbd--;
        return;
    }

    if (slotsForSumOfLengthAndLbd) {
        // Go to the right
        if (lbd < clauseLength) {
            lbd++;
            return;
        }

        // Go to the next layer
        clauseLength++;
        lbd = maxSumOfLengthAndLbd-clauseLength+1;
        return;
    }

    // Order clauses by size primary, by LBD secondary
    const int effectiveClauseLength = clauseLength - ClauseMetadata::numInts();
    if (lbd == std::max(1, effectiveClauseLength)) {
        clauseLength++;
        lbd = effectiveClauseLength+1 <= 1 ? 1 : 2;
    } else {
        lbd++;
    }
}

bool operator==(const BufferIterator& other) const {
    return clauseLength == other.clauseLength && lbd == other.lbd;
}

};

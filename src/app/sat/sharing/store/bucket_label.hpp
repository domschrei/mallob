
#pragma once

#include "util/assert.hpp"

struct BucketLabel {

    enum Mode {MINIMIZE_SIZE, MINIMIZE_SUM_OF_SIZE_AND_LBD} mode = MINIMIZE_SIZE;
    int maxLbdPartitionedSize;

    int size = 1;
    int lbd = 1;

    BucketLabel() {}
    BucketLabel(Mode mode, int maxLbdPartitionedSize) : mode(mode), 
        maxLbdPartitionedSize(maxLbdPartitionedSize) {}

    void next() {

        if (mode == MINIMIZE_SUM_OF_SIZE_AND_LBD) {
            // Beginning: Order clauses with LBD 2
            if (size < 4 && lbd <= 2) {
                size++;
                if (size == 2) lbd++; // only unit clauses have LBD 1
                assert(lbd == 2);
                return;
            }
            // Jump to the next diagonal
            if (lbd == 2) {
                size++;
                while (lbd+1 <= size-1) {
                    lbd++;
                    size--;
                }
                return;
            }
            // Go diagonally to the bottom left until LBD=2
            size++;
            lbd--;
            return;
        }

        // Order clauses by size primary, by LBD secondary
        if (mode == MINIMIZE_SIZE) {
            if (lbd == size || size > maxLbdPartitionedSize) {
                size++;
                lbd = 2;
            } else {
                lbd++;
            }
        }
    }

    bool operator==(const BucketLabel& other) {
        return size == other.size && lbd == other.lbd;
    }
    bool operator!=(const BucketLabel& other) {
        return !(*this == other);
    }
};

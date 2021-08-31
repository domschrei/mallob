
#ifndef DOMPASCH_MALLOB_BUCKET_LABEL_HPP
#define DOMPASCH_MALLOB_BUCKET_LABEL_HPP

struct BucketLabel {
    int size = 1;
    int lbd = 1;

    void next(int maxLbdPartitionedSize) {
        if (lbd == size || size > maxLbdPartitionedSize) {
            size++;
            lbd = 2;
        } else {
            lbd++;
        }
    }

    bool operator==(const BucketLabel& other) {
        return size == other.size && lbd == other.lbd;
    }
    bool operator!=(const BucketLabel& other) {
        return !(*this == other);
    }
};

#endif

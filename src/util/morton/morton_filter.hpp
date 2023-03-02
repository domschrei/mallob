
#pragma once

#include "morton_filter.h"
#include "util/morton/compressed_cuckoo_filter.h"

class MortonFilter {

private:
    // CompressedCuckoo::Morton3_8 but with resizing enabled
    typedef CompressedCuckoo::CompressedCuckooFilter<
        3, 8, 16, 512, CompressedCuckoo::target_compression_ratio_sfp_3_8,
        CompressedCuckoo::CounterReadMethodEnum::READ_SIMPLE, 
        CompressedCuckoo::FingerprintReadMethodEnum::READ_SIMPLE,
        CompressedCuckoo::ReductionMethodEnum::POP_CNT,
        CompressedCuckoo::AlternateBucketSelectionMethodEnum::FUNCTION_BASED_OFFSET,
        CompressedCuckoo::OverflowTrackingArrayHashingMethodEnum::CLUSTERED_BUCKET_HASH,
        /*resizingEnabled=*/true, true, true, true, false, true,
        CompressedCuckoo::FingerprintComparisonMethodEnum::VARIABLE_COUNT
    > GrowingMortonFilter;

    // constant size
    CompressedCuckoo::Morton3_8 _filter;
    // growing
    //GrowingMortonFilter _filter;

public:
    // constant size
    MortonFilter(size_t size) : _filter((size_t)(size / 0.95) + 64) {}
    // growing
    //MortonFilter(size_t initialSize) : _filter((size_t)(initialSize / 0.95) + 64) {}
    
    bool registerItem(uint64_t item) {
        return _filter.insert(item);
    }

    size_t getSizeInBytes() const {
        return _filter.sizeInBytes;
    }
};

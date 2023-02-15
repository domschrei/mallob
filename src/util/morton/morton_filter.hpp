
#pragma once

#include "morton_filter.h"
#include "util/morton/compressed_cuckoo_filter.h"

class MortonFilter {

private:
    CompressedCuckoo::Morton3_8 _filter;

public:
    MortonFilter(size_t size) : _filter((size_t)(size / 0.95) + 64) {}
    bool registerItem(uint64_t item) {
        
        return _filter.insert(item);
    }

    size_t getSizeInBytes() const {
        return _filter.sizeInBytes;
    }
};

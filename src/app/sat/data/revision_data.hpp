
#pragma once

#include "data/checksum.hpp"

struct RevisionData {
    size_t fSize {0};
    const int* fLits {0};
    size_t aSize {0};
    const int* aLits {0};
    Checksum chksum;
};

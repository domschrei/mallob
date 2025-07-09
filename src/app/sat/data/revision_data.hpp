
#pragma once

#include "data/checksum.hpp"

struct RevisionData {
    size_t fSize {0};
    const int* fLits {0};
    Checksum chksum;
};

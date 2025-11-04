
#pragma once

#include "data/checksum.hpp"

struct RevisionData {
    std::shared_ptr<std::vector<int>> fLits;
    Checksum chksum;
};

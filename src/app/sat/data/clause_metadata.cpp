
#include "clause_metadata.hpp"

#include "util/assert.hpp"

int ClauseMetadata::metadataSize = 0;

int ClauseMetadata::getEpoch(unsigned long id, const std::vector<unsigned long>& globalIdStartsPerEpoch) {
    // will point to 1st element >= clauseId (or end)
    auto it = std::lower_bound(globalIdStartsPerEpoch.begin(), globalIdStartsPerEpoch.end(), id);
    assert(it != globalIdStartsPerEpoch.begin());
    // point to last element < id
    --it;
    auto epoch = std::distance(std::begin(globalIdStartsPerEpoch), it);
    return epoch;
}

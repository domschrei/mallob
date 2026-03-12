
#include "clause_metadata.hpp"

#include <assert.h>
#include <algorithm>
#include <iterator>

int ClauseMetadata::metadataSize = 0;
bool ClauseMetadata::idsEnabled = false;
bool ClauseMetadata::signaturesEnabled = false;
bool ClauseMetadata::incrementalEnabled = false;

int ClauseMetadata::getEpoch(unsigned long id, const std::vector<unsigned long>& globalIdStartsPerEpoch) {
    // will point to 1st element >= clauseId (or end)
    auto it = std::lower_bound(globalIdStartsPerEpoch.begin(), globalIdStartsPerEpoch.end(), id);
    assert(it != globalIdStartsPerEpoch.begin());
    //if (it == globalIdStartsPerEpoch.end() || *it > id) {
        // point to last element < id
        --it;
    //}
    auto epoch = std::distance(std::begin(globalIdStartsPerEpoch), it);
    return epoch;
}

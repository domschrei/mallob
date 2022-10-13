
#include "clause_metadata_def.hpp"

#include "util/assert.hpp"

namespace metadata {
	unsigned long readUnsignedLong(const int* data) {
		unsigned long ul;
		memcpy(&ul, data, sizeof(unsigned long));
		return ul;
	}
	void writeUnsignedLong(unsigned long ul, int* data) {
		memcpy(data, &ul, sizeof(unsigned long));
	}
	int getEpoch(unsigned long id, const std::vector<unsigned long>& globalIdStartsPerEpoch) {
		// will point to 1st element >= clauseId (or end)
		auto it = std::lower_bound(globalIdStartsPerEpoch.begin(), globalIdStartsPerEpoch.end(), id);
		assert(it != globalIdStartsPerEpoch.begin());
		if (it == globalIdStartsPerEpoch.end() || *it > id) {
			// point to last element < id
			--it;
		}
		auto epoch = std::distance(std::begin(globalIdStartsPerEpoch), it);
		return epoch;
	}
}

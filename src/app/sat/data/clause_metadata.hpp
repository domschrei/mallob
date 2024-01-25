
#pragma once

#include <cstring>
#include <vector>

class ClauseMetadata {

	static int metadataSize;
	static bool idsEnabled;
	static bool signaturesEnabled;

public:
	static void enableClauseIds() {
		if (idsEnabled) return;
		metadataSize += 2; // 2 32-bit integers = 64 bit ID
		idsEnabled = true;
	}
	static void enableClauseSignatures() {
		if (signaturesEnabled) return;
		metadataSize += 4; // 4 32-bit integers = 128 bit signature
		signaturesEnabled = true;
	}

	static inline int numInts() {
		return metadataSize;
	}
	static inline bool enabled() {
		return metadataSize != 0;
	}
	
	static inline unsigned long readUnsignedLong(const int* data) {
		unsigned long ul;
		memcpy(&ul, data, sizeof(unsigned long));
		return ul;
	}
	static inline void writeUnsignedLong(unsigned long ul, int* data) {
		memcpy(data, &ul, sizeof(unsigned long));
	}
	
	static int getEpoch(unsigned long id, const std::vector<unsigned long>& globalIdStartsPerEpoch);
};


#include "clause_metadata_def.hpp"

namespace metadata {
	unsigned long readUnsignedLong(const int* data) {
		unsigned long ul;
		memcpy(&ul, data, sizeof(unsigned long));
		return ul;
	}
	void writeUnsignedLong(unsigned long ul, int* data) {
		memcpy(data, &ul, sizeof(unsigned long));
	}
}


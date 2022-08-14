
#pragma once

#include <cstring>
#include <vector>

#ifndef MALLOB_CLAUSE_METADATA_SIZE
#define MALLOB_CLAUSE_METADATA_SIZE 0
#endif

static_assert(MALLOB_CLAUSE_METADATA_SIZE == 0 || MALLOB_CLAUSE_METADATA_SIZE >= 2, 
    "MALLOB_CLAUSE_METADATA_SIZE must either be zero or at least two "
    "since clauses of length two including metadata are not yet supported.");

namespace metadata {
	unsigned long readUnsignedLong(const int* data);
	void writeUnsignedLong(unsigned long ul, int* data);

	int getEpoch(unsigned long id, const std::vector<unsigned long>& globalIdStartsPerEpoch);
}

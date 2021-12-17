/*
 * ClauseFilter.cpp
 *
 *  Created on: Aug 12, 2014
 *      Author: balyo
 */

#include "clause_filter.hpp"

#include <stdlib.h>
#include <stdio.h>

bool ClauseFilter::registerClause(const std::vector<int>& cls) {
	return registerClause(cls.data(), cls.size());
}

bool ClauseFilter::registerClause(const int* begin, int size) {

	// Block clauses above maximum length
	if (_max_clause_length > 0 && size > _max_clause_length) return false;

	// Clear filter if necessary
	bool isClearSet = true;
	if (_clear.compare_exchange_weak(isClearSet, false, std::memory_order_relaxed))
		clear();

	// Check all four hash functions
	bool admitted = false;
	for (int i = 1; i <= 4; i++) {
		size_t h = ClauseHasher::hash(begin, size, i) % NUM_BITS;
		// If not yet admitted, check if the bit is unset
		if (!admitted) admitted = !_bitset.test(h, std::memory_order_relaxed);
		// If admitted, set the bit
		if (admitted) _bitset.set(h, true, std::memory_order_relaxed);
	}
	return admitted;
}

void ClauseFilter::clear() {
	_bitset.reset();
}

void ClauseFilter::setClear() {
	_clear.store(true, std::memory_order_relaxed);
}

void ClauseFilter::clearHalf() {

	// The probability to delete a single bit is set to 0.1591 (or 1591/10000).
	// => P(clause is deleted) = P(one of four bits is removed) = 1 - (1-pDelete)^4 = 0.5.
	//for (size_t i = _bitset._Find_first(); i < _bitset.size(); i = _bitset._Find_next(i)) {
	//	if (rand() % 10000 < 1591) _bitset.set(i, false); // unset bit
	//}
}

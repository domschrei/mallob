/*
 * ClauseFilter.cpp
 *
 *  Created on: Aug 12, 2014
 *      Author: balyo
 */

#include "ClauseFilter.h"
#include <stdlib.h>
#include <stdio.h>

using namespace std;

#define NUM_PRIMES 16

static unsigned const int primes [] = {2038072819, 2038073287,	2038073761,	2038074317,
		2038072823,	2038073321,	2038073767,	2038074319,
		2038072847,	2038073341,	2038073789,	2038074329,
		2038074751,	2038075231,	2038075751,	2038076267};

size_t ClauseFilter::hash(const vector<int>& cls, int which, bool skipFirst) {
	return hash(cls.data(), cls.size(), which, skipFirst);
}

size_t ClauseFilter::hash(const int* begin, int size, int which, bool skipFirst) {
	size_t res = 0;
	for (auto it = begin; it != begin+size; it++) {
		if (skipFirst) {
			skipFirst = false;
			continue;
		}	
		int lit = *it;
		res ^= lit * primes[abs((lit^which) & 15)];
	}
	return res;
}

bool ClauseFilter::registerClause(const vector<int>& cls) {
	return registerClause(cls.data(), cls.size());
}

bool ClauseFilter::registerClause(const int* begin, int size) {

	if (size > 1) size--; // subtract "glue" int from total size

	// Block clauses above maximum length
	if (maxClauseLen > 0 && size > maxClauseLen) return false;

	// unit clauses are checked explicitly
	if (size == 1) { // Unit clause!
		if (checkUnits) {

			// Always admit unit clause if a check is not possible right now
			if (!unitLock.tryLock()) return true; 
			
			int firstLit = *begin;
			bool admit = !units.count(firstLit);
			if (admit) units.insert(firstLit);

			unitLock.unlock();
			return admit;
		}
		return true;
	}

	size_t h1 = hash(begin, size, 1, true) % NUM_BITS;
	size_t h2 = hash(begin, size, 2, true) % NUM_BITS;
	size_t h3 = hash(begin, size, 3, true) % NUM_BITS;
	size_t h4 = hash(begin, size, 4, true) % NUM_BITS;

	if (s1.test(h1) && s1.test(h2) && s1.test(h3) && s1.test(h4)) {
		return false;
	} else {
		s1.set(h1, true);
		s1.set(h2, true);
		s1.set(h3, true);
		s1.set(h4, true);
		return true;
	}
}

void ClauseFilter::clear() {
	s1.reset();
	auto lock = unitLock.getLock();
	units.clear();
}

void ClauseFilter::clearHalf() {

	// The probability to delete a single bit is set to 0.1591 (or 1591/10000).
	// => P(clause is deleted) = P(one of four bits is removed) = 1 - (1-pDelete)^4 = 0.5.
	for (size_t i = s1._Find_first(); i < s1.size(); i = s1._Find_next(i)) {
		if (rand() % 10000 < 1591) s1.set(i, false); // unset bit
	}

	// Remove half of all unit clauses
	std::vector<int> unitsToDelete;
	for (int unit : units) {
		if (rand() % 2 == 0) unitsToDelete.push_back(unit);
	}
	for (int unit : unitsToDelete) {
		units.erase(unit);
	}
}

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
	return hash(cls.begin(), cls.end(), which, skipFirst);
}

size_t ClauseFilter::hash(const vector<int>::const_iterator begin, const vector<int>::const_iterator end, int which, bool skipFirst) {
	size_t res = 0;
	for (auto it = begin; it != end; it++) {
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
	return registerClause(cls.begin(), cls.end());
}

bool ClauseFilter::registerClause(std::vector<int>::const_iterator begin, std::vector<int>::const_iterator end) {

	// unit clauses are checked explicitly
	auto it = begin;
	if (it++ == end) { // Unit clause!
		if (checkUnits) {

			// Always admit unit clause if a check is not possible right now
			if (!unitLock.try_lock()) return true; 
			
			int firstLit = *begin;
			bool admit = !units.count(firstLit);
			if (admit) units.insert(firstLit);

			unitLock.unlock();
			return admit;
		}
		return true;
	}

	size_t h1 = hash(begin, end, 1, true) % NUM_BITS;
	size_t h2 = hash(begin, end, 2, true) % NUM_BITS;
	size_t h3 = hash(begin, end, 3, true) % NUM_BITS;
	size_t h4 = hash(begin, end, 4, true) % NUM_BITS;

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
	auto lock = std::unique_lock<std::mutex>(unitLock);
	units.clear();
}

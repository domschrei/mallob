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

size_t ClauseFilter::commutativeHashFunctionSkipFirst(const vector<int>& cls, int which) {
	size_t res = 0;
	// skip the first int
	for (size_t j = 1; j < cls.size(); j++) {
		int lit = cls[j];
		res ^= lit * primes[abs((lit^which) & 15)];
	}
	return res;
}

size_t ClauseFilter::commutativeHashFunction(const vector<int>& cls, int which) {
	size_t res = 0;
	for (size_t j = 0; j < cls.size(); j++) {
		int lit = cls[j];
		res ^= lit * primes[abs((lit^which) & 15)];
	}
	return res;
}

ClauseFilter::ClauseFilter() {
	s1 = new bitset<NUM_BITS>();
}

ClauseFilter::~ClauseFilter() {
	delete s1;
}

bool ClauseFilter::registerClause(const vector<int>& cls) {
	// unit clauses always get in
	if (cls.size() == 1) {
		return true;
	}

	size_t h1 = commutativeHashFunctionSkipFirst(cls, 1) % NUM_BITS;
	size_t h2 = commutativeHashFunctionSkipFirst(cls, 2) % NUM_BITS;
	size_t h3 = commutativeHashFunctionSkipFirst(cls, 3) % NUM_BITS;
	size_t h4 = commutativeHashFunctionSkipFirst(cls, 4) % NUM_BITS;

	if (s1->test(h1) && s1->test(h2) && s1->test(h3) && s1->test(h4)) {
		return false;
	} else {
		s1->set(h1, true);
		s1->set(h2, true);
		s1->set(h3, true);
		s1->set(h4, true);
		return true;
	}
}

void ClauseFilter::clear() {
	s1->reset();
}

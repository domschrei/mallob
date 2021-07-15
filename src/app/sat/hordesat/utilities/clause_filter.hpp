/*
 * ClauseFilter.h
 *
 *  Created on: Aug 12, 2014
 *      Author: balyo
 */

#ifndef CLAUSEFILTER_H_
#define CLAUSEFILTER_H_

#include <vector>
#include <bitset>
#include "util/hashing.hpp"

#include "app/sat/hordesat/utilities/clause.hpp"
using namespace Mallob;

#include "util/sys/threading.hpp"

//#define NUM_BITS 268435399 // 32MB
#define NUM_BITS 26843543 // 3,2MB

class ExactSortedClauseFilter {

private:
	robin_hood::unordered_set<Clause, ClauseHasher, SortedClauseExactEquals> _set;
public:
	ExactSortedClauseFilter() {}
	bool registerClause(const Clause& c) {
		if (_set.count(c)) return false;
		_set.insert(c);
		return true;
	}
	void clear() {
		_set.clear();
	}
};

class ClauseFilter {

private:
	std::bitset<NUM_BITS> s1;
	int maxClauseLen = 0;
	bool checkUnits = false;

	robin_hood::unordered_set<int, ClauseHasher> units;
	Mutex unitLock;

public:
	ClauseFilter() : maxClauseLen(0), checkUnits(false) {}
	ClauseFilter(int maxClauseLen, bool checkUnits) : maxClauseLen(maxClauseLen), checkUnits(checkUnits) {}
	ClauseFilter(const ClauseFilter& other) : maxClauseLen(other.maxClauseLen), checkUnits(other.checkUnits) {
		s1 = other.s1;
		units = other.units;
	}
	ClauseFilter(ClauseFilter&& other) : maxClauseLen(other.maxClauseLen), checkUnits(other.checkUnits) {
		s1 = std::move(other.s1);
		units = std::move(other.units);
	}
	virtual ~ClauseFilter() {}

	/**
	 * Return false if the given clause has already been registered
	 * otherwise add it to the filter and return true.
	 */
	bool registerClause(const std::vector<int>& cls);
	bool registerClause(const int* first, int size);

	/**
	 * Clear the filter, i.e., return to its initial state.
	 */
	void clear();

	void clearHalf();
};

#endif /* CLAUSEFILTER_H_ */

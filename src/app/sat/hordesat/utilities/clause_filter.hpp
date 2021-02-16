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
#include "util/robin_hood.hpp"

#include "util/sys/threading.hpp"

//#define NUM_BITS 268435399 // 32MB
#define NUM_BITS 26843543 // 3,2MB

class ClauseFilter {

public:
	struct ClauseHasher {
		std::size_t operator()(const std::vector<int>& cls) const {
			return ClauseFilter::hash(cls, 1, cls.size() > 1);
		}
	};
	struct UnitHasher {
		std::size_t operator()(const int& unit) const {
			std::vector<int> unitCls(1, unit);
			return ClauseFilter::hash(unitCls, 1, false);
		}
	};
	struct ClauseHashBasedEquals {
		ClauseHasher _hasher;
		bool operator()(const std::vector<int>& a, const std::vector<int>& b) const {
			if (a.size() != b.size()) return false; // only clauses of same size are equal
			if (a.size() == 1) return a[0] == b[0]; // exact comparison of unit clauses
			return _hasher(a) == _hasher(b); // inexact hash-based comparison otherwise
		}
	};

private:
	std::bitset<NUM_BITS> s1;
	int maxClauseLen = 0;
	bool checkUnits = false;

	robin_hood::unordered_set<int, UnitHasher> units;
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

	/**
	 * Hash function for clauses, order of literals is irrelevant
	 */
	static size_t hash(const std::vector<int>& cls, int which, bool skipFirst);
	static size_t hash(const int* first, int size, int which, bool skipFirst);

};

#endif /* CLAUSEFILTER_H_ */

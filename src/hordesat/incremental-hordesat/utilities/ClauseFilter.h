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
#include <unordered_set>
#include <mutex>

using namespace std;

//#define NUM_BITS 268435399 // 32MB
#define NUM_BITS 26843543 // 3,2MB

class ClauseFilter {
public:
	ClauseFilter() : maxClauseLen(0), checkUnits(false) {}
	ClauseFilter(int maxClauseLen, bool checkUnits) : maxClauseLen(maxClauseLen), checkUnits(checkUnits) {}
	virtual ~ClauseFilter() {}
	/**
	 * Return false if the given clause has already been registered
	 * otherwise add it to the filter and return true.
	 */
	bool registerClause(const vector<int>& cls);
	bool registerClause(std::vector<int>::const_iterator first, std::vector<int>::const_iterator last, int size);

	/**
	 * Clear the filter, i.e., return to its initial state.
	 */
	void clear();

	/**
	 * Hash function for clauses, order of literals is irrelevant
	 */
	static size_t hash(const vector<int>& cls, int which, bool skipFirst);
	static size_t hash(const vector<int>::const_iterator first, const vector<int>::const_iterator second, int which, bool skipFirst);

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

private:
	bitset<NUM_BITS> s1;
	int maxClauseLen = 0;
	bool checkUnits = false;

	std::unordered_set<int, UnitHasher> units;
	Mutex unitLock;
};

#endif /* CLAUSEFILTER_H_ */

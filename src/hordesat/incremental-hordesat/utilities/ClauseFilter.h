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
	ClauseFilter() {}
	ClauseFilter(bool checkUnits) : checkUnits(checkUnits) {}
	virtual ~ClauseFilter() {}
	/**
	 * Return false if the given clause has already been registered
	 * otherwise add it to the filter and return true.
	 */
	bool registerClause(const vector<int>& cls);
	/**
	 * Clear the filter, i.e., return to its initial state.
	 */
	void clear();

	/**
	 * Hash function for clauses, order of literals is irrelevant, ignores first literal
	 */
	static size_t commutativeHashFunctionSkipFirst(const vector<int>& cls, int which);

	/**
	 * Hash function for clauses, order of literals is irrelevant
	 */
	static size_t commutativeHashFunction(const vector<int>& cls, int which);

private:
	bitset<NUM_BITS> s1;
	bool checkUnits = false;
	std::unordered_set<int> units;
	std::mutex unitLock;
};

#endif /* CLAUSEFILTER_H_ */

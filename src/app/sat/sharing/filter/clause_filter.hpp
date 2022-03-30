
#pragma once

#include <vector>
#include <bitset>
#include "util/hashing.hpp"

#include "../../data/clause.hpp"
using namespace Mallob;

#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "util/atomic_bitset/atomic_bitset.hpp"

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

struct AbstractClauseThreewayComparator {
	virtual int compare(const Clause& left, const Clause& right) const = 0;
};
struct LexicographicClauseThreewayComparator : public AbstractClauseThreewayComparator {
	int compare(const Clause& left, const Clause& right) const {
		// Shortest length first
		if (left.size != right.size) return left.size < right.size ? -1 : 1;
		// Shortest LBD first
		if (left.lbd != right.lbd) return left.lbd < right.lbd ? -1 : 1;
		// Lexicographic comparison of literals
		for (size_t i = 0; i < left.size; i++) {
			if (left.begin[i] != right.begin[i]) 
				return left.begin[i] < right.begin[i] ? -1 : 1;
		}
		return 0;
	}
};
struct LengthLbdSumClauseThreewayComparator : public AbstractClauseThreewayComparator {
	int maxLengthLbdSum;
	LengthLbdSumClauseThreewayComparator(int maxLengthLbdSum) : maxLengthLbdSum(maxLengthLbdSum) {}
	int compare(const Clause& left, const Clause& right) const {
		// Shortest sum of length + lbd first
		int leftSum = left.size+left.lbd;
		int rightSum = right.size+right.lbd;
		if (leftSum != rightSum && (leftSum <= maxLengthLbdSum || rightSum <= maxLengthLbdSum)) 
			return left.size+left.lbd < right.size+right.lbd ? -1 : 1;
		// Shortest length first
		if (left.size != right.size) return left.size < right.size ? -1 : 1;
		// Shortest LBD first
		if (left.lbd != right.lbd) return left.lbd < right.lbd ? -1 : 1;
		// Lexicographic comparison of literals
		for (size_t i = 0; i < left.size; i++) {
			if (left.begin[i] != right.begin[i]) 
				return left.begin[i] < right.begin[i] ? -1 : 1;
		}
		return 0;
	}
};
struct ClauseComparator {
	AbstractClauseThreewayComparator* compare;
	ClauseComparator(AbstractClauseThreewayComparator* compare) : compare(compare) {}
	bool operator()(const Clause& left, const Clause& right) const {
		return compare->compare(left, right) == -1;
	}
};

/*
Comment D. Schreiber 09/2021

In a Bloom filter, we use k distinct hash functions to set specific bits of a bitset of size m
in order to "register" (query and insert) n elements.

In our case, we set m=26843543 and k=4. Depending on the number n of inserted elements
we get the following false positive probabilities:

n		  	  p
     1'000 	  4.92891e-16
    10'000 	  4.91571e-12
   100'000 	  4.78579e-08
   500'000    2.65734e-05
 1'000'000	  0.00036
10'000'000 	  0.36010

Generally: p = (1 - e^(-k*n/m))^k
Source: https://en.wikipedia.org/wiki/Bloom_filter#Probability_of_false_positives

For a low number of threads (1-16), the number of clauses exported within a second
is not expected to surpass anything near a million, at which point an expected number 
of 360 clauses would be spuriously discarded. At 500'000 clauses (still a high number)
we expect a much lower number of 13 spuriously discarded clauses, and at below 300'000
clauses we expect this for less than a single clause to occur. However, a high thoughput
of clauses does imply that the clause filter needs to be cleaned up quite frequently
or we otherwise need to put up with noticeable false positive rates.

In addition, we can reduce the number of inserted clauses by only registering a learnt
clause in a filter if there is (probably) still space for the clause in the database structure.
*/

//#define NUM_BITS 268435399 // 32MB
#define NUM_BITS 26843543 // 3,2MB

class ClauseFilter {

private:
	//std::bitset<NUM_BITS> _bitset;
	AtomicBitset _bitset;
	int _max_clause_length = 0;
	std::atomic_bool _clear{false};

public:
	ClauseFilter() : _bitset(NUM_BITS), _max_clause_length(0) {}
	ClauseFilter(int maxClauseLen) : _bitset(NUM_BITS), _max_clause_length(maxClauseLen) {}
	ClauseFilter(const ClauseFilter& other) : _bitset(other._bitset), _max_clause_length(other._max_clause_length) {}
	ClauseFilter(ClauseFilter&& other) : _bitset(std::move(other._bitset)), _max_clause_length(other._max_clause_length) {}
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

	void setClear();

	void clearHalf();
};

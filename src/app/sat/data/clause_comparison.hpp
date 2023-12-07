
#pragma once

#include "app/sat/data/clause.hpp"
using namespace Mallob;

struct AbstractClauseThreewayComparator {
	virtual ~AbstractClauseThreewayComparator() {}
	virtual int compare(const Clause& left, const Clause& right) const = 0;
};

struct LexicographicClauseThreewayComparator : public AbstractClauseThreewayComparator {
	int compare(const Clause& left, const Clause& right) const {
		// Shortest length first
		if (left.size != right.size) return left.size < right.size ? -1 : 1;
		// Shortest LBD first
		if (left.lbd != right.lbd) return left.lbd < right.lbd ? -1 : 1;
		// Lexicographic comparison of literals
		for (size_t i = ClauseMetadata::numInts(); i < left.size; i++) {
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
		for (size_t i = ClauseMetadata::numInts(); i < left.size; i++) {
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

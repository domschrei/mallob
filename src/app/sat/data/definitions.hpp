
#pragma once

#include <functional>  // for function
namespace Mallob { struct Clause; }

enum SatResult {
	SAT = 10,
	UNSAT = 20,
	IMPROVED = 40,
	UNKNOWN = 0
};

typedef std::function<void(const Mallob::Clause&, int)> LearnedClauseCallback;
typedef std::function<void(const Mallob::Clause&, int, int, const std::vector<int>&)> ExtLearnedClauseCallback;
typedef std::function<bool(int)> ProbingLearnedClauseCallback;

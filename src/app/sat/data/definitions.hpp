
#pragma once

#include "app/sat/data/clause.hpp"

enum SatResult {
	SAT = 10,
	UNSAT = 20,
	UNKNOWN = 0
};

typedef std::function<void(const Mallob::Clause&, int)> LearnedClauseCallback;
typedef std::function<void(const Mallob::Clause&, int, int, int)> ExtLearnedClauseCallback;
typedef std::function<bool(int)> ProbingLearnedClauseCallback;

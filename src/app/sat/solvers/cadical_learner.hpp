
#pragma once

#include "util/assert.hpp"

#include "cadical_interface.hpp"
#include "portfolio_solver_interface.hpp"
using namespace Mallob;

struct HordeLearner : public CaDiCaL::Learner {

private:
	const SolverSetup& _setup;
	LearnedClauseCallback _callback;

	Clause _current_clause;
	std::vector<int> _current_lits;
	
	int _glue_limit;
	unsigned long _num_produced;
	
public:
	HordeLearner(const SolverSetup& setup) : _setup(setup), 
			_current_lits(1+setup.strictClauseLengthLimit, 0), 
			_glue_limit(_setup.strictLbdLimit) {
		
		_current_clause.begin = _current_lits.data()+1;
		_current_clause.size = 0;
	}
	~HordeLearner() override {}

  	bool learning(int size) override {
		return size > 0 && size <= _setup.strictClauseLengthLimit;
	}

	inline void learn(int lit) override {

		if (lit != 0) {
			// Received a literal
			assert(_current_clause.size < 1+_setup.strictClauseLengthLimit);
			_current_lits[_current_clause.size++] = lit;
			return;
		} 

		// Received a zero - clause is finished
		_num_produced++;

		bool eligible = true;
		if (_current_clause.size > 1) {
			assert(_current_clause.size >= 3); // glue value plus at least two literals
			// subtract LBD value which was added to the clause length as well
			_current_clause.size--; 
			// Non-unit clause: First integer is glue value.
			// In CaDiCaL, LBD scores are represented from 1 to len-1. => Increment LBD.
			_current_clause.lbd = _current_lits[0]+1;
			if (_current_clause.lbd > _glue_limit) eligible = false;
		} else {
			_current_clause.lbd = 1;
			_current_lits[1] = _current_lits[0]; // copy only literal to position 1
		}
		
		// Export clause (if eligible), reset current clause
		if (eligible) _callback(_current_clause, _setup.localId);
		_current_clause.size = 0;
	}

    void setCallback(const LearnedClauseCallback& callback) {
        _callback = callback;
    }

	unsigned long getNumProduced() const {
		return _num_produced;
	}
};
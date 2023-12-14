
#pragma once

#include <fstream>

#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "util/assert.hpp"

#include "cadical/src/cadical.hpp"
#include "portfolio_solver_interface.hpp"
using namespace Mallob;

struct CadicalClauseExport : public CaDiCaL::Learner {

private:
	const SolverSetup& _setup;
	LearnedClauseCallback _callback;

	Clause _current_clause;
	std::vector<int> _current_lits;
	
	int _glue_limit;
	unsigned long _num_produced;

	uint64_t _last_id;
	
public:
	CadicalClauseExport(const SolverSetup& setup) : _setup(setup), 
			_current_lits(2+setup.strictClauseLengthLimit, 0),
			_glue_limit(_setup.strictLbdLimit), _last_id(setup.numOriginalClauses) {

		_current_clause.begin = _current_lits.data();
		_current_clause.size = ClauseMetadata::numInts();
	}
	~CadicalClauseExport() override {}

  	bool learning(int size) override {
		return size > 0 && size <= _setup.strictClauseLengthLimit;
	}

	void append_literal(int lit) override {
		// Received a literal
		assert(_current_clause.size - ClauseMetadata::numInts() < _setup.strictClauseLengthLimit);
		_current_lits[_current_clause.size++] = lit;
	}

	void publish_clause (uint64_t id, int glue) override {
		_num_produced++;
		assert(!ClauseMetadata::enabled() || id > _last_id);
		_last_id = id;

		bool eligible = true;
		_current_clause.lbd = glue;
		// Increment LBD of non-unit clauses with LBD 1
		if (_current_clause.size > ClauseMetadata::numInts()+1
				&& _current_clause.lbd == 1)
			++_current_clause.lbd;
		if (_current_clause.lbd > _glue_limit) eligible = false;

		if (ClauseMetadata::enabled()) {
			LOG(V5_DEBG, "EXPORT ID=%ld len=%i\n", id, _current_clause.size - ClauseMetadata::numInts());
			memcpy(_current_clause.begin, &id, sizeof(uint64_t));
		}
		assert(_current_clause.size > ClauseMetadata::numInts());

		// Export clause (if eligible), reset current clause
		if (eligible) _callback(_current_clause, _setup.localId);
		_current_clause.size = ClauseMetadata::numInts();
	}

    void setCallback(const LearnedClauseCallback& callback) {
        _callback = callback;
    }

	unsigned long getNumProduced() const {
		return _num_produced;
	}
};
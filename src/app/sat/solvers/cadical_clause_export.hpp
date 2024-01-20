
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
	ProbingLearnedClauseCallback _probing_callback;

	Clause _current_clause;
	std::vector<int> _current_lits;
	
	int _glue_limit;
	unsigned long _num_produced;

	uint64_t _last_id;
	bool _sign_shared_clauses {false};
	
public:
	CadicalClauseExport(const SolverSetup& setup) : _setup(setup), 
			_current_lits(2+8+setup.strictMaxLitsPerClause, 0),
			_glue_limit(_setup.strictLbdLimit), _last_id(setup.numOriginalClauses),
			_sign_shared_clauses(_setup.onTheFlyChecking) {

		_current_clause.begin = _current_lits.data();
		_current_clause.size = ClauseMetadata::numInts();
	}
	~CadicalClauseExport() override {}

  	bool learning(int size) override {
		if (size <= 0) return false;
		if (size > _setup.strictMaxLitsPerClause) return false;
		int effectiveSize = size + ClauseMetadata::numInts();
		if (!_probing_callback(effectiveSize)) return false;
		return true;
	}

	void append_literal(int lit) override {
		// Received a literal
		assert(_current_clause.size - ClauseMetadata::numInts() < _setup.strictMaxLitsPerClause);
		_current_lits[_current_clause.size++] = lit;
	}

	void publish_clause (uint64_t id, int glue, const uint8_t* signatureData, int signatureSize) override {
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
			ClauseMetadata::writeUnsignedLong(id, _current_clause.begin);
			if (_sign_shared_clauses) {
				memcpy(_current_clause.begin+2, signatureData, signatureSize);
			}
		}
		LOG(V5_DEBG, "EXPORT ID=%ld len=%i %s\n", id,
			_current_clause.size - ClauseMetadata::numInts(), _current_clause.toStr().c_str());
		assert(_current_clause.size > ClauseMetadata::numInts());

		// Export clause (if eligible), reset current clause
		if (eligible) _callback(_current_clause, _setup.localId);
		_current_clause.size = ClauseMetadata::numInts();
	}

    void setCallback(const LearnedClauseCallback& callback) {
        _callback = callback;
    }

	void setProbingCallback(const ProbingLearnedClauseCallback& callback) {
		_probing_callback = callback;
	}

	unsigned long getNumProduced() const {
		return _num_produced;
	}
};
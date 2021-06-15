
#ifndef DOMPASCH_MALLOB_MALLOB_LEARN_SOURCE_HPP
#define DOMPASCH_MALLOB_MALLOB_LEARN_SOURCE_HPP

#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "util/ringbuffer.hpp"

struct MallobLearnSource : public CaDiCaL::LearnSource {
private:
    Logger& _log;

    UnitClauseRingBuffer _learned_units;
	MixedNonunitClauseRingBuffer _learned_clauses;

    std::vector<int> _intermediate_unit_buffer;
    std::vector<int> _intermediate_buffer;
    std::vector<int> _next_clause;

	unsigned long _num_received = 0;
	unsigned long _num_discarded = 0;
    unsigned long _num_digested = 0;

public:
    MallobLearnSource(const SolverSetup& setup) : 
            _log(*setup.logger),
            _learned_clauses(4*setup.anticipatedLitsToImportPerCycle),
            _learned_units(2*setup.anticipatedLitsToImportPerCycle+1) {

    }
    ~MallobLearnSource() { }

    void addUnit(int lit) {
        if (!_learned_units.insertUnit(lit)) {
            _num_discarded++;
        }
        _num_received++;
    }
    void addClause(const int* begin, size_t size) {
        if (!_learned_clauses.insertClause(begin, size, 0)) {
            _num_discarded++;
        }
        _num_received++;
    }

    bool hasNextClause() override {
        // Clauses left in intermediate unit buffer?
        if (!_intermediate_unit_buffer.empty()) return true;
        // Clauses left in intermediate buffer?
        if (!_intermediate_buffer.empty()) return true;
        // Try to refill buffer with unit clause ringbuffer
        if (_learned_units.getUnits(_intermediate_unit_buffer)) return true;
        // Try to refill buffer with clause ringbuffer
        return _learned_clauses.getClause(_intermediate_buffer);
    }
    const std::vector<int>& getNextClause() override {

        // Report unit clause if possible
        if (!_intermediate_unit_buffer.empty()) {
            _next_clause.resize(1);
            _next_clause[0] = _intermediate_unit_buffer.back();
            _intermediate_unit_buffer.pop_back();
            //_log.log(V4_VVER, "Import unit clause to CaDiCaL\n");
            return _next_clause;
        }

        // Extract a non-unit clause
        size_t pos = _intermediate_buffer.size()-1;
        assert(_intermediate_buffer[pos] == 0);
        do {pos--;} while (pos > 0 && _intermediate_buffer[pos-1] != 0);
        assert(pos == 0 || pos >= 3); // glue int, >= two literals, separation zero
        _next_clause.resize(_intermediate_buffer.size()-pos-1);
        // Write clause into buffer
        memcpy(_next_clause.data(), _intermediate_buffer.data()+pos, _next_clause.size()*sizeof(int));
        assert(_next_clause[0] > 0);
        for (size_t i = 1; i < _next_clause.size(); i++) assert(_next_clause[i] != 0);

        _intermediate_buffer.resize(pos);
        assert(_intermediate_buffer.empty() || _intermediate_buffer[pos-1] == 0);
        _num_digested++;
        
        //_log.log(V4_VVER, "Import clause of size %lu to CaDiCaL\n", _next_clause.size());
        return _next_clause;
    }
};

#endif

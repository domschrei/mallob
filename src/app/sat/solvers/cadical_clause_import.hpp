
#pragma once

#include "cadical/src/cadical.hpp"
#include "portfolio_solver_interface.hpp"
#include "util/sys/atomics.hpp"

struct CadicalClauseImport : public CaDiCaL::LearnSource {

private:
    Logger& _log;
    std::function<Mallob::Clause()> _clause_fetcher;
    std::vector<int> _next_clause;
    uint64_t _next_id {0};
    int _next_glue;

public:
    CadicalClauseImport(const SolverSetup& setup, std::function<Mallob::Clause()> clauseFetcher) : 
        _log(*setup.logger),
        _clause_fetcher(clauseFetcher) {}
    ~CadicalClauseImport() { }

    bool hasNextClause() override {
        
        auto clause = _clause_fetcher();
        if (clause.begin == nullptr) return false;
        
        assert(clause.size >= ClauseMetadata::numBytes()+1);
        
        if (clause.size == ClauseMetadata::numBytes()+1) {
            // Unit clause
            _next_clause.resize(ClauseMetadata::numBytes()+1);
            for (size_t i = 0; i < _next_clause.size(); ++i) {
                _next_clause[i] = clause.begin[i];
            }
            if (ClauseMetadata::enabled()) {
                memcpy(&_next_id, clause.begin, sizeof(uint64_t));
                LOG(V5_DEBG, "IMPORT ID=%ld len=%i\n", _next_id, 1);
            }
            _next_glue = 1;
            return true;
        }

        // Non-unit clause
        if (ClauseMetadata::enabled()) {
            memcpy(&_next_id, clause.begin, sizeof(uint64_t));
            LOG(V5_DEBG, "IMPORT ID=%ld len=%i\n", _next_id, clause.size-2);
        }
        _next_glue = clause.lbd;
        _next_clause.resize(clause.size);
        for (size_t i = 0; i < clause.size; i++) {
            _next_clause[i] = clause.begin[i];
        }
        return true;
    }
    const std::vector<int>& getNextClause(uint64_t& id, int& glue) override {
        id = _next_id;
        glue = _next_glue;
        return _next_clause;
    }
};


#pragma once

#include "app/sat/data/clause_metadata.hpp"
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
        
        assert(clause.size >= ClauseMetadata::numInts()+1);
        
        if (ClauseMetadata::enabled()) {
            memcpy(&_next_id, clause.begin, sizeof(uint64_t));
            LOG(V5_DEBG, "IMPORT ID=%ld len=%i\n", _next_id, clause.size);
        }
        _next_clause.resize(clause.size - ClauseMetadata::numInts());
        for (size_t i = ClauseMetadata::numInts(); i < clause.size; ++i) {
            _next_clause[i-ClauseMetadata::numInts()] = clause.begin[i];
        }
        _next_glue = clause.lbd;
        return true;
    }
    const std::vector<int>& getNextClause(uint64_t& id, int& glue) override {
        id = _next_id;
        glue = _next_glue;
        return _next_clause;
    }
};

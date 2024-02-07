
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
    std::vector<uint8_t> _next_signature {32};
    bool _sign_shared_clauses {false};

public:
    CadicalClauseImport(const SolverSetup& setup, std::function<Mallob::Clause()> clauseFetcher) : 
        _log(*setup.logger),
        _clause_fetcher(clauseFetcher),
        _sign_shared_clauses(ClauseMetadata::numInts() > 2) {

        if (_sign_shared_clauses) {
            _next_signature.resize(sizeof(int) * (ClauseMetadata::numInts() - 2));
        }
    }
    ~CadicalClauseImport() { }

    bool hasNextClause() override {
        
        auto clause = _clause_fetcher();
        if (clause.begin == nullptr) return false;
        
        assert(clause.size >= ClauseMetadata::numInts()+1);
        assert(clause.lbd <= clause.size - ClauseMetadata::numInts());
        
        if (ClauseMetadata::enabled()) {
            _next_id = ClauseMetadata::readUnsignedLong(clause.begin);
            if (_sign_shared_clauses) {
                memcpy(_next_signature.data(), clause.begin+2, sizeof(int) * (ClauseMetadata::numInts()-2));
            }
        }
        LOG(V5_DEBG, "IMPORT ID=%ld len=%i %s\n", _next_id,
            clause.size - ClauseMetadata::numInts(), clause.toStr().c_str());
        _next_clause.resize(clause.size - ClauseMetadata::numInts());
        for (size_t i = ClauseMetadata::numInts(); i < clause.size; ++i) {
            _next_clause[i-ClauseMetadata::numInts()] = clause.begin[i];
        }
        _next_glue = clause.lbd;
        return true;
    }
    const std::vector<int>& getNextClause(uint64_t& id, int& glue, std::vector<uint8_t>*& signature) override {
        id = _next_id;
        glue = _next_glue;
        signature = &_next_signature;
        assert(glue <= _next_clause.size());
        return _next_clause;
    }
};

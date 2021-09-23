
#ifndef DOMPASCH_MALLOB_MALLOB_LEARN_SOURCE_HPP
#define DOMPASCH_MALLOB_MALLOB_LEARN_SOURCE_HPP


#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "util/ringbuffer.hpp"
#include "util/sys/atomics.hpp"
#include "app/sat/hordesat/sharing/adaptive_clause_database.hpp"
#include "app/sat/hordesat/sharing/import_buffer.hpp"

struct MallobLearnSource : public CaDiCaL::LearnSource {

private:
    Logger& _log;
    std::function<Mallob::Clause()> _clause_fetcher;
    std::vector<int> _next_clause;

public:
    MallobLearnSource(const SolverSetup& setup, std::function<Mallob::Clause()> clauseFetcher) : 
        _log(*setup.logger),
        _clause_fetcher(clauseFetcher) {}
    ~MallobLearnSource() { }

    bool hasNextClause() override {
        
        auto clause = _clause_fetcher();
        if (clause.begin == nullptr) return false;
        
        if (clause.size == 1) {
            _next_clause.resize(1);
            _next_clause[0] = clause.begin[0];
            return true;
        }

        _next_clause.resize(clause.size+1);
        _next_clause[0] = clause.lbd;
        for (size_t i = 0; i < clause.size; i++) {
            _next_clause[i+1] = clause.begin[i];
        }
        return true;
    }
    const std::vector<int>& getNextClause() override {
        return _next_clause;
    }
};

#endif

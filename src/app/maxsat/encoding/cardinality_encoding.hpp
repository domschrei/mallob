
#pragma once

#include "app/maxsat/maxsat_instance.hpp"
#include "util/logger.hpp"
#include <functional>

void cardinality_encoding_add_literal(int lit, void* instance);
void cardinality_encoding_add_assumption(int lit, void* instance);

class CardinalityEncoding {
public:
    CardinalityEncoding(unsigned int nbVars, const std::vector<MaxSatInstance::ObjectiveTerm>& objective) : _nb_vars(nbVars) {}
    void setClauseCollector(std::function<void(int)> clauseCollector) {
        _clause_collector = clauseCollector;
    }
    void setAssumptionCollector(std::function<void(int)> assumptionCollector) {
        _assumption_collector = assumptionCollector;
    }
    void encode(size_t lb, size_t ub, size_t max) {
        //int guardVar = prepareGuardVariable();
        doEncode(lb, ub, max);
        //addGuardClauseIfNeeded(guardVar);
    }
    void enforceBound(size_t bound) {
        //int guardVar = prepareGuardVariable();
        doEnforce(bound);
        //addGuardClauseIfNeeded(guardVar);
    }
    virtual ~CardinalityEncoding() {}
protected:
    unsigned int _nb_vars;
    std::function<void(int)> _clause_collector;
    std::function<void(int)> _assumption_collector;
    virtual void doEncode(size_t min, size_t ub, size_t max) = 0;
    virtual void doEnforce(size_t bound) = 0;
    int prepareGuardVariable() {
        // Introduce a meaningless variable to potentially reference later.
        _nb_vars++;
        return _nb_vars;
    }
    void addGuardClauseIfNeeded(int guardVar) {
        // Did the number of variables change since selecting the guard variable?
        if (_nb_vars == guardVar) return;
        // Introduce a (so far) meaningless variable that represents the max. variable.
        _nb_vars++;
        // Add the constraint "the prior guard var or ... or the last var is true".
        // This can't break anything even if more variables are added later on,
        // since the prior guard var can always be set to true, and it
        // explicitly adds all variables to the encoding.
        for (int lit = guardVar; lit <= _nb_vars; lit++)
            addLiteral(lit);
        addLiteral(0);
    }
private:
    void addLiteral(int lit) {
        LOG(V6_DEBGV, "CARDI ADD %i\n", lit);
        _clause_collector(lit);
    }
    void addAssumption(int lit) {
        LOG(V6_DEBGV, "CARDI ASSUME %i\n", lit);
        _assumption_collector(lit);
    }
friend void cardinality_encoding_add_literal(int lit, void* instance);
friend void cardinality_encoding_add_assumption(int lit, void* instance);
};

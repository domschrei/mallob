
#pragma once

#include "app/maxsat/maxsat_instance.hpp"
#include <functional>

void cardinality_encoding_add_literal(int lit, void* instance);
void cardinality_encoding_add_assumption(int lit, void* instance);

class CardinalityEncoding {
public:
    CardinalityEncoding(unsigned int& nbVars, const std::vector<MaxSatInstance::ObjectiveTerm>& objective) : _nb_vars(nbVars) {}
    void encode(size_t lb, size_t ub, std::function<void(int)> clauseCollector) {
        _clause_collector = clauseCollector;
        doEncode(lb, ub);
    }
    void enforceBound(size_t bound, std::function<void(int)> assumptionCollector) {
        _assumption_collector = assumptionCollector;
        doEnforce(bound);
    }
    virtual ~CardinalityEncoding() {}
protected:
    unsigned int& _nb_vars;
    std::function<void(int)> _clause_collector;
    std::function<void(int)> _assumption_collector;
    virtual void doEncode(size_t lb, size_t ub) = 0;
    virtual void doEnforce(size_t bound) = 0;
private:
    void addLiteral(int lit) {
        _clause_collector(lit);
    }
    void addAssumption(int lit) {
        _assumption_collector(lit);
    }
friend void cardinality_encoding_add_literal(int lit, void* instance);
friend void cardinality_encoding_add_assumption(int lit, void* instance);
};


#pragma once

#include <functional>

#include "app/maxsat/encoding/cardinality_encoding.hpp"
#include "app/maxsat/encoding/openwbo/enc_adder.hpp"
#include "app/maxsat/maxsat_instance.hpp"
#include "util/logger.hpp"

class WarnersAdder : public CardinalityEncoding {
private:
    openwbo::Adder _enc;
    struct SolverProxy : public openwbo::Adder::Solver {
        WarnersAdder* parent;
        unsigned int& nbVars;
        SolverProxy(WarnersAdder* parent, unsigned int& nbVars) : parent(parent), nbVars(nbVars) {}
        virtual int freshVariable() override {
            nbVars++;
            return nbVars;
        }
        virtual void pushLiteral(int lit) override {
            assert(std::abs(lit) <= nbVars);
            cardinality_encoding_add_literal(lit, parent);
        }
    } _solver;
    std::vector<int> _lits;
    std::vector<size_t> _weights;
    size_t _sum_of_weights;

public:
    WarnersAdder(unsigned int nbVars, const std::vector<MaxSatInstance::ObjectiveTerm>& objective) : CardinalityEncoding(nbVars, objective),
            _solver(this, _nb_vars) {
        for (auto& term : objective) {
            _lits.push_back(term.lit);
            _weights.push_back(term.factor);
            _sum_of_weights += term.factor;
        }
    }
    virtual void doEncode(size_t min, size_t ub, size_t max) override {
        if (_enc.hasCreatedEncoding()) return;
        auto internalLits = _enc.convertLiterals(_lits);
        // We need to always encode the adder up until the total sum of all coefficients.
        // Otherwise, costs above the provided "max" are *not* being forbidden and can
        // be reported as valid models by a solver.
        size_t rhs = std::max(max, _sum_of_weights);
        _enc.encode(&_solver, internalLits, _weights, rhs);
    }
    virtual void doEnforce(size_t bound) override {
        assert(_enc.hasCreatedEncoding());
        openwbo::Adder::vec<openwbo::Lit> assumptions;
        _enc.updateInc(&_solver, bound, assumptions);
        for (auto a : assumptions)
            cardinality_encoding_add_assumption(openwbo::toExternalLit(a), this);
    }
    virtual ~WarnersAdder() {}
};


#pragma once

#include <functional>

#include "app/maxsat/encoding/cardinality_encoding.hpp"
#include "app/maxsat/maxsat_instance.hpp"
#include "rustsat.h"
#include "util/logger.hpp"

class PolynomialWatchdog : public CardinalityEncoding {
private:
    RustSAT::DynamicPolyWatchdog* _enc {nullptr};
public:
    PolynomialWatchdog(unsigned int nbVars, const std::vector<MaxSatInstance::ObjectiveTerm>& objective) : CardinalityEncoding(nbVars, objective) {
        _enc = RustSAT::dpw_new();
        for (auto& term : objective) RustSAT::dpw_add(_enc, term.lit, term.factor);
        RustSAT::dpw_reserve(_enc, &_nb_vars);
    }
    virtual void doEncode(size_t min, size_t ub, size_t max) override {
        //max = std::max(max, ub);
        assert(min <= ub && ub <= max);
        RustSAT::dpw_limit_range(_enc, min, max,
            &cardinality_encoding_add_literal, this);
        RustSAT::dpw_encode_ub(_enc, ub, ub, &_nb_vars,
            &cardinality_encoding_add_literal, this);
    }
    virtual void doEnforce(size_t bound) override {
        RustSAT::dpw_enforce_ub(_enc, bound, &cardinality_encoding_add_assumption, this);
    }
    virtual ~PolynomialWatchdog() {
        RustSAT::dpw_drop(_enc);
    }
};

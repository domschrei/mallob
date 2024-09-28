
#pragma once

#include <functional>

#include "app/maxsat/encoding/cardinality_encoding.hpp"
#include "app/maxsat/maxsat_instance.hpp"
#include "rustsat.h"

class GeneralizedTotalizer : public CardinalityEncoding {
private:
    RustSAT::DbGte* _enc {nullptr};
public:
    GeneralizedTotalizer(unsigned int& nbVars, const std::vector<MaxSatInstance::ObjectiveTerm>& objective) : CardinalityEncoding(nbVars, objective) {
        _enc = RustSAT::gte_new();
        for (auto& term : objective) RustSAT::gte_add(_enc, term.lit, term.factor);
    }
    virtual void doEncode(size_t lb, size_t ub, size_t max) override {
        RustSAT::gte_encode_ub(_enc, lb, ub, &_nb_vars,
            &cardinality_encoding_add_literal, this);
    }
    virtual void doEnforce(size_t bound) override {
        RustSAT::gte_enforce_ub(_enc, bound, &cardinality_encoding_add_assumption, this);
    }
    virtual ~GeneralizedTotalizer() {
        RustSAT::gte_drop(_enc);
    }
};

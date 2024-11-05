
#pragma once

#include <algorithm>
#include <string>
#include <cstring>

#include "app/sat/data/theories/integer_term.hpp"
#include "data/serializable.hpp"
#include "util/assert.hpp"

struct IntegerRule : public Serializable {
    enum RuleType {MINIMIZE, MAXIMIZE, INVARIANT} type;
    std::string inner;
    IntegerTerm term1;
    IntegerTerm term2;

    IntegerRule() {}
    IntegerRule(RuleType type, const std::string& inner, IntegerTerm&& left, IntegerTerm&& right) :
        type(type), inner(inner), term1(left), term2(right) {}
    IntegerRule(RuleType type, IntegerTerm&& term) :
        type(type), term1(term) {}

    std::string toStr() const {
        if (type == MINIMIZE) return "min " + term1.toStr();
        if (type == MAXIMIZE) return "max " + term1.toStr();
        if (type == INVARIANT) return "inv " + term1.toStr() + " " + inner + " " + term2.toStr();
        return "ERR";
    }

    virtual std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> out;
        out.insert(out.end(), (uint8_t*) (&type), (uint8_t*) ((&type)+1));
        auto termPacked = term1.serialize();
        out.insert(out.end(), termPacked.begin(), termPacked.end());
        if (type == INVARIANT) {
            std::string comp = inner;
            while (comp.size() < 2) comp += " ";
            assert(comp.size() == 2);
            out.insert(out.end(), comp.begin(), comp.begin()+2);
            auto termPacked = term2.serialize();
            out.insert(out.end(), termPacked.begin(), termPacked.end());
        }
        return out;
    }
    virtual IntegerRule& deserialize(const std::vector<uint8_t>& packed) override {
        assert(packed.size() >= sizeof(type));
        memcpy(&type, packed.data(), sizeof(type));
        int pos = sizeof(type);
        pos += term1.deserialize(packed, sizeof(type));
        if (type == INVARIANT) {
            char op[3];
            memcpy(op, packed.data() + pos, 2);
            op[2] = '\0';
            inner = op;
            inner.erase(std::remove_if(inner.begin(), inner.end(), ::isspace), inner.end());
            pos += 2;
            pos += term2.deserialize(packed, pos);
            assert(pos == packed.size());
        }
        return *this;
    }
};
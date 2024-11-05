
#pragma once

#include <cmath>
#include <string>
#include <variant>

#include "data/serializable.hpp"
#include "util/assert.hpp"
#include "util/compression.hpp"

struct IntegerTerm : public Serializable {
    enum Type {
        NONE, LITERAL, CONSTANT,
        NEGATE, ADD, SUBTRACT, MULTIPLY, DIVIDE, MODULO, POW, LOG,
        BIG_ADD, BIG_MULTIPLY,
    } type {NONE};
    std::variant<long, std::vector<IntegerTerm>> content;

    IntegerTerm() {
        setType(NONE);
    }
    IntegerTerm(Type type) {
        setType(type);
    }

    void setType(Type type) {
        this->type = type;
        if (isPrimitive()) content = 0L;
        else content = std::vector<IntegerTerm>();
    }

    bool isPrimitive() const {
        return type == NONE || type == LITERAL || type == CONSTANT;
    }
    bool isFixedSize() const {
        return isPrimitive() || (type != BIG_ADD && type != BIG_MULTIPLY);
    }

    inline std::vector<IntegerTerm>& children() {return std::get<std::vector<IntegerTerm>>(content);}
    inline const std::vector<IntegerTerm>& children() const {return std::get<std::vector<IntegerTerm>>(content);}

    inline long& inner() {return std::get<long>(content);}
    inline const long& inner() const {return std::get<long>(content);}

    std::string toStr() const {
        switch (type) {
        case NONE: return "";
        case CONSTANT: return std::to_string(inner());
        case LITERAL: return (inner() > 0 ? "." : "!") + std::to_string(std::abs(inner()));
        case NEGATE: return "(-" + children().front().toStr() + ")";
        case SUBTRACT: return "(" + children()[0].toStr() + "-" + children()[1].toStr() + ")";
        case DIVIDE: return "(" + children()[0].toStr() + "/" + children()[1].toStr() + ")";
        case MODULO: return "(" + children()[0].toStr() + "%" + children()[1].toStr() + ")";
        case POW: return "(" + children()[0].toStr() + "^" + children()[1].toStr() + ")";
        case ADD:
        case MULTIPLY: 
        case BIG_ADD:
        case BIG_MULTIPLY: {
            /* k-ary operations */
            std::string op = (type==ADD || type==BIG_ADD) ? "+" : "*";
            std::string out = "(";
            for (auto& term : children()) out += term.toStr() + op;
            return out.substr(0, out.size()-1) + ")";
        }
        default: return "ERR";
        }
    }

    /*
    The "values" argument can be any struct that supports the following function:
    long operator[](long idx) const;
    where "idx" can be any variable which is featured as a literal in the expression.
    */
    template <typename T>
    long evaluate(const T& values) const {
        switch (type) {
        case NONE: return 0;
        case CONSTANT: return inner();
        case LITERAL: return (inner()>0 ? 1 : -1) * values[std::abs(inner())];
        case NEGATE: return -1 * children()[0].evaluate(values);
        case SUBTRACT: return children()[0].evaluate(values) + children()[1].evaluate(values);
        case DIVIDE: return children()[0].evaluate(values) / children()[1].evaluate(values);
        case MODULO: return children()[0].evaluate(values) % children()[1].evaluate(values);
        case POW: return std::pow(children()[0].evaluate(values), children()[1].evaluate(values));
        case ADD: case BIG_ADD: {
            long sum = 0;
            for (auto& term : children()) sum += term.evaluate(values);
            return sum;
        }
        case MULTIPLY: case BIG_MULTIPLY: {
            long prod = 1;
            for (auto& term : children()) prod *= term.evaluate(values);
            return prod;
        }
        default: return 0;
        }
    }

    void addChildAndTryFlatten(IntegerTerm&& child) {
        if ((type == ADD || type == BIG_ADD) && (child.type == ADD || child.type == BIG_ADD)) {
            if (children().size() + child.children().size() > 2) type = BIG_ADD;
            children().insert(children().end(), child.children().begin(), child.children().end());
            return;
        }
        if ((type == MULTIPLY || type == BIG_MULTIPLY) && (child.type == MULTIPLY || child.type == BIG_MULTIPLY)) {
            if (children().size() + child.children().size() > 2) type = BIG_MULTIPLY;
            children().insert(children().end(), child.children().begin(), child.children().end());
            return;
        }
        if (type == NEGATE && child.isPrimitive()) {
            setType(child.type);
            inner() = -1 * child.inner();
            return;
        }
        children().push_back(std::move(child));
        if (type == ADD && children().size() > 2) type = BIG_ADD;
        if (type == MULTIPLY && children().size() > 2) type = BIG_MULTIPLY;
    }

    virtual std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> out;
        serialize(out);
        return out;
    }
    void serialize(std::vector<uint8_t>& out) const {
        int before = out.size();

        // type
        char t = (char) type;
        out.insert(out.end(), &t, (&t)+1);

        if (isPrimitive()) {
            // data
            uint8_t data[10];
            int len = toVariableBytelength(inner(), data);
            out.insert(out.end(), data, data+len);
        } else if (isFixedSize()) {
            // data
            assert(!children().empty());
            assert(children().size() <= 2);
            assert(type != NEGATE || children().size() == 1);
            for (const auto& term : children()) {
                term.serialize(out);
            }
        } else {
            // # children
            int nbChildren = children().size();
            uint8_t data[5];
            int len = toVariableBytelength(nbChildren, data);
            out.insert(out.end(), data, data+len);

            // data
            for (const auto& term : children()) {
                term.serialize(out);
            }
        }

    }

    virtual IntegerTerm& deserialize(const std::vector<uint8_t>& packed) override {
        deserialize(packed, 0);
        return *this;
    }
    int deserialize(const std::vector<uint8_t>& packed, int begin) {
        // type
        char t;
        memcpy(&t, packed.data()+begin, 1);
        type = (Type) t;
        setType(type);

        if (isPrimitive()) {
            // data
            int len;
            inner() = fromVariableBytelength<long>(packed.data()+begin+1, len);
            return 1 + len;
        } else {
            // # children
            int nbChildren, lenOfNbChildren;
            if (isFixedSize()) {
                nbChildren = 1;
                if (type != NEGATE) nbChildren++;
                lenOfNbChildren = 0;
            } else {
                nbChildren = fromVariableBytelength<int>(packed.data()+begin+1, lenOfNbChildren);
            }

            // data
            int pos = begin + 1 + lenOfNbChildren;
            for (int childIdx = 0; childIdx < nbChildren; childIdx++) {
                IntegerTerm child;
                pos += child.deserialize(packed, pos);
                children().push_back(std::move(child));
            }
            return pos - begin; // number of deserialized bytes
        }
    }
};
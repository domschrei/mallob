
#pragma once

#include "clause.hpp"
#include "util/assert.hpp"
#include "util/tsl/robin_map.h"

struct ProducedUnitClause {
    int literal = 0;

    ProducedUnitClause() = default;
    ProducedUnitClause(const Mallob::Clause& cls) {
        assert(cls.size == 1);
        assert(cls.lbd == 1);
        literal = cls.begin[0];
    }

    ProducedUnitClause extractUnsafe() const {
        ProducedUnitClause c;
        c.literal = literal;
        return c;
    }

    bool valid() const {return literal != 0;}

    bool operator<(const ProducedUnitClause& other) const {
        return literal < other.literal;
    }
    bool operator==(const ProducedUnitClause& other) const {
        return literal == other.literal;
    }
    bool operator!=(const ProducedUnitClause& other) const {
        return !(*this == other);
    }
};

struct ProducedBinaryClause {
    int literals[2];
    
    ProducedBinaryClause() {
        literals[0] = 0;
        literals[1] = 1;
    }
    ProducedBinaryClause(const Mallob::Clause& cls) {
        assert(cls.size == 2);
        assert(cls.lbd == 2);
        literals[0] = cls.begin[0];
        literals[1] = cls.begin[1];
    }

    ProducedBinaryClause extractUnsafe() const {
        ProducedBinaryClause c;
        c.literals[0] = literals[0];
        c.literals[1] = literals[1];
        return c;
    }

    bool valid() const {return literals[0] != 0;}

    bool operator<(const ProducedBinaryClause& other) const {
        if (literals[0] != other.literals[0]) return literals[0] < other.literals[0];
        return literals[1] < other.literals[1];
    }
    bool operator==(const ProducedBinaryClause& other) const {
        if (literals[0] != other.literals[0]) return false;
        return literals[1] == other.literals[1];
    }
    bool operator!=(const ProducedBinaryClause& other) const {
        return !(*this == other);
    }
};

struct ProducedLargeClause {
    
    uint16_t size;
    // This member is marked mutable in order to allow extraction of a clause from a hash table.
    mutable int* data = nullptr;

    ProducedLargeClause() = default;
    ProducedLargeClause(const Mallob::Clause& cls) {
        assert(cls.size < 256);
        size = cls.size;
        data = (int*) malloc(sizeof(int) * size);
        memcpy(data, cls.begin, sizeof(int) * size);
    }
    ProducedLargeClause(ProducedLargeClause&& moved) {
        *this = std::move(moved);
    }
    ProducedLargeClause(const ProducedLargeClause& other) {
        size = other.size;
        data = (int*) malloc(sizeof(int) * size);
        memcpy(data, other.data, sizeof(int) * size);
    }

    // This method allows for the extraction of a clause from a hash table.
    ProducedLargeClause extractUnsafe() const {
        ProducedLargeClause c;
        c.size = size;
        c.data = data;
        data = nullptr;
        return c;
    }

    bool valid() const {return data != nullptr;}

    ProducedLargeClause& operator=(ProducedLargeClause&& moved) {
        size = moved.size;
        data = moved.data;
        moved.data = nullptr;
        return *this;
    }
    ~ProducedLargeClause() {
        if (data != nullptr) free(data);
    }

    bool operator<(const ProducedLargeClause& other) const {
        if (size != other.size) return size < other.size;
        for (uint8_t i = 0; i < size; i++) {
            if (data[i] != other.data[i]) return data[i] < other.data[i];
        }
        return false;
    }
    bool operator==(const ProducedLargeClause& other) const {
        if (size != other.size) return false;
        for (uint8_t i = 0; i < size; i++) {
            if (data[i] != other.data[i]) return false;
        }
        return true;
    }
    bool operator!=(const ProducedLargeClause& other) const {
        return !(*this == other);
    }
};

namespace prod_cls {

    template<typename T>
    uint8_t size(const T& producedClause) {
        if constexpr (std::is_base_of<ProducedUnitClause, T>()) {
            return 1;
        }
        if constexpr (std::is_base_of<ProducedBinaryClause, T>()) {
            return 2;
        }
        if constexpr (std::is_base_of<ProducedLargeClause, T>()) {
            return producedClause.size;
        }
    }

    template<typename T>
    const int* data(const T& producedClause) {
        if constexpr (std::is_base_of<ProducedUnitClause, T>()) {
            return &producedClause.literal;
        }
        if constexpr (std::is_base_of<ProducedBinaryClause, T>()) {
            return producedClause.literals;
        }
        if constexpr (std::is_base_of<ProducedLargeClause, T>()) {
            return producedClause.data;
        }
    }

    template<typename T>
    int* data(T& producedClause) {
        if constexpr (std::is_base_of<ProducedUnitClause, T>()) {
            return &producedClause.literal;
        }
        if constexpr (std::is_base_of<ProducedBinaryClause, T>()) {
            return producedClause.literals;
        }
        if constexpr (std::is_base_of<ProducedLargeClause, T>()) {
            return producedClause.data;
        }
    }

    template<typename T>
    Mallob::Clause toMallobClause(T& producedClause) {
        if constexpr (std::is_base_of<ProducedUnitClause, T>()) {
            return Mallob::Clause(&producedClause.literal, 1, 1);
        }
        if constexpr (std::is_base_of<ProducedBinaryClause, T>()) {
            int* data = producedClause.literals;
            return Mallob::Clause(data, 2, 2);
        }
        if constexpr (std::is_base_of<ProducedLargeClause, T>()) {
            int* data = producedClause.data;
            return Mallob::Clause(data, producedClause.size, -1);
        }
    }

    template<typename T>
    Mallob::Clause toMallobClause(T&& producedClause) {
        if constexpr (std::is_base_of<ProducedUnitClause, T>()) {
            int* data = (int*) malloc(sizeof(int));
            data[0] = producedClause.literal;
            producedClause.literal = 0;
            return Mallob::Clause(data, 1, 1);
        }
        if constexpr (std::is_base_of<ProducedBinaryClause, T>()) {
            int* data = (int*) malloc(sizeof(int) * 2);
            data[0] = producedClause.literals[0];
            data[1] = producedClause.literals[1];
            producedClause.literals[0] = 0;
            return Mallob::Clause(data, 2, 2);
        }
        if constexpr (std::is_base_of<ProducedLargeClause, T>()) {
            int* data = producedClause.data;
            producedClause.data = nullptr;
            return Mallob::Clause(data, producedClause.size, -1);
        }
    }

    template<typename T>
    int serializedSize(const T& producedClause, bool explicitLbd) {
        if constexpr (std::is_base_of<ProducedUnitClause, T>()) {
            return 1;
        }
        if constexpr (std::is_base_of<ProducedBinaryClause, T>()) {
            return 2;
        }
        if constexpr (std::is_base_of<ProducedLargeClause, T>()) {
            return (explicitLbd ? 1 : 0) + producedClause.size;
        }
    }

    template<typename T>
    void append(const T& producedClause, std::vector<int>& out, int explicitLbdOrZero) {
        if constexpr (std::is_base_of<ProducedUnitClause, T>()) {
            out.push_back(producedClause.literal);
        }
        if constexpr (std::is_base_of<ProducedBinaryClause, T>()) {
            out.push_back(producedClause.literals[0]);
            out.push_back(producedClause.literals[1]);
        }
        if constexpr (std::is_base_of<ProducedLargeClause, T>()) {
            if (explicitLbdOrZero != 0) out.push_back(explicitLbdOrZero);
            for (size_t i = 0; i < producedClause.size; i++)
                out.push_back(producedClause.data[i]);
        }
    }

    template<typename T>
    std::string toStr(const T& producedClause, int explicitLbdOrZero) {
        if constexpr (std::is_base_of<ProducedUnitClause, T>()) {
            return "len=1 lbd=1 " + std::to_string(producedClause.literal);
        }
        if constexpr (std::is_base_of<ProducedBinaryClause, T>()) {
            return "len=2 lbd=2 " + std::to_string(producedClause.literals[0]) + " " + std::to_string(producedClause.literals[0]) ;
        }
        if constexpr (std::is_base_of<ProducedLargeClause, T>()) {
            std::string out = "len=" + std::to_string(producedClause.size) + " lbd=" + std::to_string(explicitLbdOrZero) + " ";
            for (size_t i = 0; i < producedClause.size; i++)
                out += std::to_string(producedClause.data[i]) + " ";
            return out;
        }
    }
}

template <typename T>
struct ProducedClauseHasher {
    std::size_t inline operator()(const T& producedClause) const {
        assert(producedClause.valid());
        return Mallob::commutativeHash(prod_cls::data(producedClause), prod_cls::size(producedClause), 3);
    }
};

template <typename T>
struct ProducedClauseEquals {
    bool inline operator()(const T& a, const T& b) const {
        if (prod_cls::size(a) != prod_cls::size(b)) return false; // only clauses of same size are equal
        // exact content comparison otherwise
        auto dataA = prod_cls::data(a);
        auto dataB = prod_cls::data(b);
        for (size_t i = 0; i < prod_cls::size(a); i++) {
            if (dataA[i] != dataB[i]) return false;
        }
        return true;
    }
};


template <typename T>
struct ProducedClauseEqualsCommutative {
    // Implemented in such a way that a successful query on sorted clauses is still linear.
    // An unsuccessful query on sorted clauses may incur quadratic work in the worst case.
    // On unsorted clauses, the worst-case complexity is quadratic in both cases. 
    bool inline operator()(const T& a, const T& b) const {
        
        auto size = prod_cls::size(a);
        if (size != prod_cls::size(b)) return false; // only clauses of same size are equal
        
        // content comparison otherwise
        auto dataA = prod_cls::data(a);
        auto dataB = prod_cls::data(b);

        // Invariant: All literals of B to the left of this index are already matched
        size_t idxB = 0; 

        // For each literal of A:
        for (size_t i = 0; i < size; i++) {
            // Same literal as at B's current position?
            if (dataA[i] == dataB[idxB]) {
                ++idxB; // this literal is checked, proceed to next one
                continue;
            }
            // Try to find the same literal within B
            // (no need to check the already matched literals)
            bool sameLitFound = false;
            for (size_t j = idxB+1; j < size; j++) {
                if (dataA[i] == dataB[j]) {
                    // Literal found
                    sameLitFound = true;
                    break;
                }
            }
            // No such literal found
            if (!sameLitFound) return false;
        }
        return true;
    }
};

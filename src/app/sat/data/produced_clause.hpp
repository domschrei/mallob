
#pragma once

#include "clause.hpp"

struct __attribute__ ((packed)) ProducedClause {
    
    // This member is marked mutable in order to allow extraction of a clause from a hash table.
    int* data = nullptr;
    uint16_t size;

    ProducedClause() = default;
    ProducedClause(const Mallob::Clause& cls) {
        assert(cls.size < 256);
        size = cls.size;
        data = (int*) malloc(sizeof(int) * size);
        memcpy(data, cls.begin, sizeof(int) * size);
    }
    ProducedClause(ProducedClause&& moved) {
        *this = std::move(moved);
    }
    ProducedClause(const ProducedClause& other) {
        *this = other;   
    }

    bool valid() const {return data != nullptr;}

    ProducedClause& operator=(ProducedClause&& moved) {
        size = moved.size;
        data = moved.data;
        moved.data = nullptr;
        return *this;
    }
    ProducedClause& operator=(const ProducedClause& other) {
        size = other.size;
        if (other.valid()) {
            data = (int*) malloc(sizeof(int) * size);
            memcpy(data, other.data, sizeof(int) * size);
        } else data = nullptr;
        return *this;
    }

    Mallob::Clause toMallobClause() {
        return Mallob::Clause(data, size, -1);
    }

    ~ProducedClause() {
        if (data != nullptr) free(data);
    }

    bool operator<(const ProducedClause& other) const {
        if (size != other.size) return size < other.size;
        for (uint8_t i = ClauseMetadata::numInts(); i < size; i++) {
            if (data[i] != other.data[i]) return data[i] < other.data[i];
        }
        return false;
    }
    bool operator==(const ProducedClause& other) const {
        if (size != other.size) return false;
        for (uint8_t i = ClauseMetadata::numInts(); i < size; i++) {
            if (data[i] != other.data[i]) return false;
        }
        return true;
    }
    bool operator!=(const ProducedClause& other) const {
        return !(*this == other);
    }

    int serializedSize(bool explicitLbd) {
        return (explicitLbd ? 1 : 0) + size;
    }

    void append(std::vector<int>& out, int explicitLbdOrZero) {
        if (explicitLbdOrZero != 0) out.push_back(explicitLbdOrZero);
        for (size_t i = 0; i < size; i++)
            out.push_back(data[i]);
    }

    std::string toStr(int explicitLbdOrZero) {
        std::string out = "len=" + std::to_string(size - ClauseMetadata::numInts())
            + " lbd=" + std::to_string(explicitLbdOrZero) + " ";
        if (ClauseMetadata::enabled()) {
            unsigned long id; memcpy(&id, data, sizeof(unsigned long));
            out += "id=" + std::to_string(id) + " ";
        }
        for (size_t i = ClauseMetadata::numInts(); i < size; i++)
            out += std::to_string(data[i]) + " ";
        return out;
    }
};

struct ProducedClauseHasher {
    std::size_t inline operator()(const ProducedClause& producedClause) const {
        assert(producedClause.valid());
        return Mallob::commutativeHash(producedClause.data, producedClause.size, 3);
    }
};

struct ProducedClauseEquals {
    bool inline operator()(const ProducedClause& a, const ProducedClause& b) const {
        if (a.size != b.size) return false; // only clauses of same size are equal
        // exact content comparison otherwise
        auto dataA = a.data;
        auto dataB = b.data;
        for (size_t i = ClauseMetadata::numInts(); i < a.size; i++) {
            if (dataA[i] != dataB[i]) return false;
        }
        return true;
    }
};

struct ProducedClauseEqualsCommutative {
    // Implemented in such a way that a successful query on sorted clauses is still linear.
    // An unsuccessful query on sorted clauses may incur quadratic work in the worst case.
    // On unsorted clauses, the worst-case complexity is quadratic in both cases. 
    bool inline operator()(const ProducedClause& a, const ProducedClause& b) const {
        
        auto size = a.size;
        if (size != b.size) return false; // only clauses of same size are equal
        
        // content comparison otherwise
        auto dataA = a.data;
        auto dataB = b.data;

        // Invariant: All literals of B to the left of this index are already matched
        size_t idxB = ClauseMetadata::numInts();

        // For each literal of A:
        for (size_t i = ClauseMetadata::numInts(); i < size; i++) {
            assert(idxB < size);
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

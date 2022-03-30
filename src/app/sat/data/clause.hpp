
#pragma once

#include <string>
#include <vector>
#include <cstring>

#include "util/assert.hpp"
#include "util/hashing.hpp"

namespace Mallob {
    
    struct Clause {
        int* begin = nullptr; 
        int size = 0; 
        int lbd = 0;

        Clause() = default;
        Clause(int* begin, int size, int lbd) : begin(begin), size(size), lbd(lbd) {}

        Clause copy() const {
            if (begin == nullptr) return Clause(nullptr, size, lbd);
            Clause c((int*)malloc(size*sizeof(int)), size, lbd);
            memcpy(c.begin, begin, size*sizeof(int));
            return c;
        }

        void assertNonZeroLiterals() const {
            for (int i = 0; i < size; i++) assert(begin[i] != 0);
        }
        std::string toStr() const {
            std::string out = "(len=" + std::to_string(size) + " lbd=" + std::to_string(lbd) + ") ";
            for (auto it = begin; it != begin+size; it++) {
                out += std::to_string(*it) + " ";
            }
            return out.substr(0, out.size()-1);
        }

        bool operator<(const Clause& other) const {
            if (size != other.size) return size < other.size;
            if (lbd != other.lbd) return lbd < other.lbd;
            for (int i = 0; i < size; i++) {
                if (begin[i] != other.begin[i]) return begin[i] < other.begin[i];
            }
            return false;
        }
    };

    inline size_t commutativeHash(const int* begin, int size, int which = 3) {
        static unsigned const int primes [] = 
            {2038072819, 2038073287, 2038073761, 2038074317,
            2038072823,	2038073321,	2038073767, 2038074319,
            2038072847,	2038073341,	2038073789,	2038074329,
            2038074751,	2038075231,	2038075751,	2038076267};
        
        size_t res = 1;
        for (auto it = begin; it != begin+size; it++) {
            int lit = *it;
            res ^= lit * primes[abs((lit^which) & 15)];
        }
        return res;
    }

    /*
    inline size_t qualityHash(const int* begin, int size, int which = 3) {
        size_t res = robin_hood::hash_int(size + which);
        for (auto it = begin; it != begin+size; it++) {
            int lit = *it;
            res ^= robin_hood::hash_int(lit); //* primes[abs((lit^which) & 15)];
        }
        return res;
    }
    */

    inline size_t nonCommutativeHash(const int* begin, int size, int which = 3) {
        
        size_t res = robin_hood::hash_int(size * which);
        for (size_t i = 0; i < size; i++) {
            hash_combine(res, begin[i]);
        }
        return res;
    }

    struct NonCommutativeClauseHasher {
        std::size_t inline operator()(const Clause& cls) const {
            return nonCommutativeHash(cls.begin, cls.size);
        }
    };

    struct ClauseHasher {

        static inline size_t hash(const std::vector<int>& cls, int which) {
            return hash(cls.data(), cls.size(), which);
        }
        static inline size_t hash(const Clause& cls, int which) {
            return hash(cls.begin, cls.size, which);
        }

        static inline size_t hash(const int* begin, int size, int which) {
            //return Mallob::fastHash(begin, size, which);
            return Mallob::commutativeHash(begin, size, which);
        }

        std::size_t inline operator()(const std::vector<int>& cls) const {
            return hash(cls, 3);
        }
        std::size_t inline operator()(const Clause& cls) const {
            return hash(cls.begin, cls.size, 3);
        }
        std::size_t inline operator()(const int& unit) const {
            return hash(&unit, 1, 3);
        }
    };

    struct SortedClauseExactEquals {
        bool operator()(const Clause& a, const Clause& b) const {
            if (a.size != b.size) return false; // only clauses of same size are equal
            // exact content comparison otherwise
            for (size_t i = 0; i < a.size; i++) {
                if (a.begin[i] != b.begin[i]) return false;
            }
            return true;
        }
    };
}

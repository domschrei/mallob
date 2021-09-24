
#ifndef DOMPASCH_MALLOB_CLAUSE_HPP
#define DOMPASCH_MALLOB_CLAUSE_HPP

#include <string>
#include <vector>
#include <cstring>

#include "util/assert.hpp"

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
            std::string out = "(lbd=" + std::to_string(lbd) + ") ";
            for (auto it = begin; it != begin+size; it++) {
                out += std::to_string(*it) + " ";
            }
            return out.substr(0, out.size()-1);
        }
    };

    struct ClauseHasher {

        static inline size_t hash(const std::vector<int>& cls, int which) {
            return hash(cls.data(), cls.size(), which);
        }

        static inline size_t hash(const int* begin, int size, int which) {
            static unsigned const int primes [] = 
                    {2038072819, 2038073287, 2038073761, 2038074317,
                    2038072823,	2038073321,	2038073767, 2038074319,
                    2038072847,	2038073341,	2038073789,	2038074329,
                    2038074751,	2038075231,	2038075751,	2038076267};
        
            size_t res = 0;
            for (auto it = begin; it != begin+size; it++) {
                int lit = *it;
                res ^= lit * primes[abs((lit^which) & 15)];
            }
            return res;
        }

        std::size_t inline operator()(const std::vector<int>& cls) const {
            return hash(cls, 3);
        }
        std::size_t inline operator()(const Clause& cls) const {
            return hash(cls.begin, cls.size, 3);
        }
        std::size_t inline operator()(const int& unit) const {
            std::vector<int> unitCls(1, unit);
            return hash(unitCls, 3);
        }
    };

    struct ClauseHashBasedInexactEquals {
        ClauseHasher _hasher;
        bool operator()(const std::vector<int>& a, const std::vector<int>& b) const {
            if (a.size() != b.size()) return false; // only clauses of same size are equal
            if (a.size() == 1) return a[0] == b[0]; // exact comparison of unit clauses
            return _hasher(a) == _hasher(b); // inexact hash-based comparison otherwise
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

#endif

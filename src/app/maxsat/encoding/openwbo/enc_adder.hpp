/*!
 * \author Ruben Martins - ruben@sat.inesc-id.pt
 *
 * @section LICENSE
 *
 * Open-WBO, Copyright (c) 2013-2018, Ruben Martins, Vasco Manquinho, Ines Lynce
 * PBLib,    Copyright (c) 2012-2013  Peter Steinke
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Edited and adapted by D. Schreiber 2024
 */

#ifndef Enc_Adder_h
#define Enc_Adder_h

#include <cmath>
#include <cstdint>
#include <map>
#include <queue>
#include <utility>
#include <vector>

#include "util/assert.hpp"

namespace openwbo {

typedef int Var;
#define var_Undef (-1)

struct Lit {
  int x;
  bool operator==(Lit p) const { return x == p.x; }
  bool operator!=(Lit p) const { return x != p.x; }
  bool operator<(Lit p) const {
    return x < p.x;
  } // '<' makes p, ~p adjacent in the ordering.
};
inline Lit mkLit(Var var, bool sign) {
  Lit p;
  p.x = var + var + (int)sign;
  return p;
}
inline Lit operator~(Lit p) {
  Lit q;
  q.x = p.x ^ 1;
  return q;
}
inline Lit operator^(Lit p, bool b) {
  Lit q;
  q.x = p.x ^ (unsigned int)b;
  return q;
}
inline bool sign(Lit p) { return p.x & 1; }
inline int var(Lit p) { return p.x >> 1; }

const Lit lit_Undef = {-2}; // }- Useful special constants.
const Lit lit_Error = {-1}; // }

// Mapping Literals to and from compact integers suitable for array indexing:
inline int toInt(Var v) { assert(v >= 0); return v; }
inline int toInt(Lit p) { assert(p != lit_Undef); assert(p != lit_Error); return p.x; }
inline Lit toLit(int i) {
  Lit p;
  p.x = i;
  assert(p != lit_Undef);
  assert(p != lit_Error);
  return p;
}
inline Lit fromExternalLit(int l) {
  Var v = std::abs(l) - 1;
  assert(v >= 0);
  assert(l != 0);
  bool sign = l > 0;
  return mkLit(v, sign);
}
inline int toExternalLit(Lit p) {
  assert(p != lit_Undef);
  assert(p != lit_Error);
  int converted = (sign(p) ? 1 : -1) * (var(p) + 1);
  assert(converted != 0);
  assert(fromExternalLit(converted) == p);
  return converted;
}

// const Lit lit_Undef = mkLit(var_Undef, false);  // }- Useful special
// constants. const Lit lit_Error = mkLit(var_Undef, true );  // }

class Adder {

public:
  // TODO
  struct Solver {
    virtual int freshVariable() = 0;
    virtual void pushLiteral(int lit) = 0;
  };
  template <class T> using vec = std::vector<T>;

public:
  bool hasEncoding;
  void addBinaryClause(Solver *SC, Lit a, Lit b) {
    assert(clause.empty());
    SC->pushLiteral(toExternalLit(a));
    SC->pushLiteral(toExternalLit(b));
    SC->pushLiteral(0);
  }
  void addTernaryClause(Solver *SC, Lit a, Lit b, Lit c) {
    assert(clause.empty());
    SC->pushLiteral(toExternalLit(a));
    SC->pushLiteral(toExternalLit(b));
    SC->pushLiteral(toExternalLit(c));
    SC->pushLiteral(0);
  }
  void addQuaternaryClause(Solver *SC, Lit a, Lit b, Lit c, Lit d) {
    assert(clause.empty());
    SC->pushLiteral(toExternalLit(a));
    SC->pushLiteral(toExternalLit(b));
    SC->pushLiteral(toExternalLit(c));
    SC->pushLiteral(toExternalLit(d));
    SC->pushLiteral(0);
  }
  void addClause(Solver *SC, vec<Lit> &clause) {
    for (Lit a : clause)
      SC->pushLiteral(toExternalLit(a));
    SC->pushLiteral(0);
  }

public:
  Adder() { hasEncoding = false; }
  ~Adder() {}

  vec<Lit> convertLiterals(const std::vector<int> &externalLits) {
    vec<Lit> out;
    for (int l : externalLits)
      out.push_back(fromExternalLit(l));
    return out;
  }

  // Encode constraint.
  void encode(Solver *SC, vec<Lit> &lits, vec<uint64_t> &coeffs, uint64_t rhs);

  // Update constraint.
  void update(Solver *SC, uint64_t rhs);

  // Returns true if the encoding was built, otherwise returns false;
  bool hasCreatedEncoding() { return hasEncoding; }

  void encodeInc(Solver *SC, vec<Lit> &lits, vec<uint64_t> &coeffs,
                 uint64_t rhs, vec<Lit> &assumptions);
  void updateInc(Solver *SC, uint64_t rhs, vec<Lit> &assumptions);

protected:
  vec<Lit> _output;
  vec<Lit> clause;
  std::vector<std::queue<Lit>> _buckets;

  void FA_extra(Solver *SC, Lit xc, Lit xs, Lit a, Lit b, Lit c);
  Lit FA_carry(Solver *SC, Lit a, Lit b, Lit c);
  Lit FA_sum(Solver *SC, Lit a, Lit b, Lit c);
  Lit HA_carry(Solver *SC, Lit a, Lit b);
  Lit HA_sum(Solver *SC, Lit a, Lit b);
  void adderTree(Solver *SC, std::vector<std::queue<Lit>> &buckets,
                 vec<Lit> &result);
  void lessThanOrEqual(Solver *SC, vec<Lit> &xs, std::vector<uint64_t> &ys);
  void numToBits(std::vector<uint64_t> &bits, uint64_t n, uint64_t number);
  uint64_t ld64(const uint64_t x);

  void lessThanOrEqualInc(Solver *SC, vec<Lit> &xs, std::vector<uint64_t> &ys,
                          vec<Lit> &assumptions);
};
} // namespace openwbo

#endif

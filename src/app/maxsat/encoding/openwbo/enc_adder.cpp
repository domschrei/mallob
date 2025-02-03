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

#include <algorithm>

#include "enc_adder.hpp"

using namespace openwbo;

void Adder::FA_extra(Solver *SC, Lit xc, Lit xs, Lit a, Lit b, Lit c) {

  clause.clear();
  addTernaryClause(SC, ~xc, ~xs, a);
  addTernaryClause(SC, ~xc, ~xs, b);
  addTernaryClause(SC, ~xc, ~xs, c);

  addTernaryClause(SC, xc, xs, ~a);
  addTernaryClause(SC, xc, xs, ~b);
  addTernaryClause(SC, xc, xs, ~c);
}

Lit Adder::FA_carry(Solver *SC, Lit a, Lit b, Lit c) {

  Lit x = mkLit(SC->freshVariable() - 1, false);

  addTernaryClause(SC, b, c, ~x);
  addTernaryClause(SC, a, c, ~x);
  addTernaryClause(SC, a, b, ~x);

  addTernaryClause(SC, ~b, ~c, x);
  addTernaryClause(SC, ~a, ~c, x);
  addTernaryClause(SC, ~a, ~b, x);

  return x;
}

Lit Adder::FA_sum(Solver *SC, Lit a, Lit b, Lit c) {
  Lit x = mkLit(SC->freshVariable() - 1, false);

  addQuaternaryClause(SC, a, b, c, ~x);
  addQuaternaryClause(SC, a, ~b, ~c, ~x);
  addQuaternaryClause(SC, ~a, b, ~c, ~x);
  addQuaternaryClause(SC, ~a, ~b, c, ~x);

  addQuaternaryClause(SC, ~a, ~b, ~c, x);
  addQuaternaryClause(SC, ~a, b, c, x);
  addQuaternaryClause(SC, a, ~b, c, x);
  addQuaternaryClause(SC, a, b, ~c, x);

  return x;
}

Lit Adder::HA_carry(Solver *SC, Lit a, Lit b) // a AND b
{
  Lit x = mkLit(SC->freshVariable() - 1, false);

  addBinaryClause(SC, a, ~x);
  addBinaryClause(SC, b, ~x);
  addTernaryClause(SC, ~a, ~b, x);

  return x;
}

Lit Adder::HA_sum(Solver *SC, Lit a, Lit b) // a XOR b
{
  Lit x = mkLit(SC->freshVariable() - 1, false);

  addTernaryClause(SC, ~a, ~b, ~x);
  addTernaryClause(SC, a, b, ~x);

  addTernaryClause(SC, ~a, b, x);
  addTernaryClause(SC, a, ~b, x);

  return x;
}

void Adder::adderTree(Solver *SC, std::vector<std::queue<Lit>> &buckets,
                      vec<Lit> &result) {
  Lit x, y, z;
  Lit u = lit_Undef;

  for (int i = 0; i < buckets.size(); i++) {
    if (buckets[i].size() == 0)
      continue;

    if (i == buckets.size() - 1 && buckets[i].size() >= 2) {
      buckets.push_back(std::queue<Lit>());
      result.push_back(u);
    }

    while (buckets[i].size() >= 3) {
      x = buckets[i].front();
      buckets[i].pop();
      y = buckets[i].front();
      buckets[i].pop();
      z = buckets[i].front();
      buckets[i].pop();
      Lit xs = FA_sum(SC, x, y, z);
      Lit xc = FA_carry(SC, x, y, z);
      buckets[i].push(xs);
      buckets[i + 1].push(xc);
      FA_extra(SC, xc, xs, x, y, z);
    }

    if (buckets[i].size() == 2) {
      x = buckets[i].front();
      buckets[i].pop();
      y = buckets[i].front();
      buckets[i].pop();
      buckets[i].push(HA_sum(SC, x, y));
      buckets[i + 1].push(HA_carry(SC, x, y));
    }

    result[i] = buckets[i].front();
    buckets[i].pop();
  }
}

// Generates clauses for “xs <= ys”, assuming ys has only constant signals (0 or
// 1).
// xs and ys must have the same size

void Adder::lessThanOrEqual(Solver *SC, vec<Lit> &xs,
                            std::vector<uint64_t> &ys) {
  assert(xs.size() == ys.size());
  vec<Lit> clause;
  bool skip;
  for (int i = 0; i < xs.size(); ++i) {
    if (ys[i] == 1 || xs[i] == lit_Undef)
      continue;

    clause.clear();

    skip = false;

    for (int j = i + 1; j < xs.size(); ++j) {
      if (ys[j] == 1) {
        if (xs[j] == lit_Undef) {
          skip = true;
          break;
        }

        clause.push_back(~xs[j]);
      } else {
        assert(ys[j] == 0);

        if (xs[j] == lit_Undef)
          continue;

        clause.push_back(xs[j]);
      }
    }

    if (skip)
      continue;

    clause.push_back(~xs[i]);

    // formula.addClause( clause );
    addClause(SC, clause);
  }
}

void Adder::lessThanOrEqualInc(Solver *SC, vec<Lit> &xs,
                               std::vector<uint64_t> &ys,
                               vec<Lit> &assumptions) {
  assert(xs.size() == ys.size());
  vec<Lit> clause;
  bool skip;
  for (int i = 0; i < xs.size(); ++i) {
    if (ys[i] == 1 || xs[i] == lit_Undef)
      continue;

    clause.clear();

    skip = false;

    for (int j = i + 1; j < xs.size(); ++j) {
      if (ys[j] == 1) {
        if (xs[j] == lit_Undef) {
          skip = true;
          break;
        }

        clause.push_back(~xs[j]);
      } else {
        assert(ys[j] == 0);

        if (xs[j] == lit_Undef)
          continue;

        clause.push_back(xs[j]);
      }
    }

    if (skip)
      continue;

    clause.push_back(~xs[i]);

    // formula.addClause( clause );
    Lit t = mkLit(SC->freshVariable() - 1, false);
    clause.push_back(t);
    assumptions.push_back(~t);
    addClause(SC, clause);
  }
}

void Adder::numToBits(std::vector<uint64_t> &bits, uint64_t n,
                      uint64_t number) {
  bits.clear();

  for (int64_t i = n - 1; i >= 0; --i) {
    int64_t tmp = ((int64_t)1) << i;
    if (number < tmp) {
      bits.push_back(0);
    } else {
      bits.push_back(1);
      number -= tmp;
    }
  }

  reverse(bits.begin(), bits.end());
}

void Adder::encode(Solver *SC, vec<Lit> &lits, vec<uint64_t> &coeffs,
                   uint64_t rhs) {

  _output.clear();

  uint64_t nb = ld64(rhs); // number of bits
  Lit u = lit_Undef;

  for (int iBit = 0; iBit < nb; ++iBit) {
    _buckets.push_back(std::queue<Lit>());
    _output.push_back(u);
    for (int iVar = 0; iVar < lits.size(); ++iVar) {
      if (((((int64_t)1) << iBit) & coeffs[iVar]) != 0)
        _buckets.back().push(lits[iVar]);
    }
  }

  std::vector<uint64_t> kBits;

  adderTree(SC, _buckets, _output);

  numToBits(kBits, _buckets.size(), rhs);

  lessThanOrEqual(SC, _output, kBits);
  hasEncoding = true;
}

void Adder::encodeInc(Solver *SC, vec<Lit> &lits, vec<uint64_t> &coeffs,
                      uint64_t rhs, vec<Lit> &assumptions) {
  _output.clear();

  uint64_t nb = ld64(rhs); // number of bits
  Lit u = lit_Undef;

  for (int iBit = 0; iBit < nb; ++iBit) {
    _buckets.push_back(std::queue<Lit>());
    _output.push_back(u);
    for (int iVar = 0; iVar < lits.size(); ++iVar) {
      if (((((int64_t)1) << iBit) & coeffs[iVar]) != 0)
        _buckets.back().push(lits[iVar]);
    }
  }

  std::vector<uint64_t> kBits;

  adderTree(SC, _buckets, _output);
  numToBits(kBits, _buckets.size(), rhs);

  lessThanOrEqualInc(SC, _output, kBits, assumptions);
  hasEncoding = true;
}


void Adder::encodeBitwiseAssumableBoundConstraint(Solver *SC, vec<Lit> &bitLitsOfCountedBound, vec<Lit> &bitLitsOfEnforcedBound) {
  assert(bitLitsOfCountedBound.size() == bitLitsOfEnforcedBound.size());
  assert(bitLitsOfCountedBound.size() == _buckets.size());

  // Encode equality variables for each bit position
  vec<Lit> equalityLits;
  for (int bitPos = 0; bitPos < bitLitsOfCountedBound.size(); ++bitPos) {
    // If the corresponding literal is undefined, we skip the bit position.
    if (bitLitsOfCountedBound[bitPos] == lit_Undef) {
      equalityLits.push_back(lit_Undef);
      continue;
    }
    assert(bitLitsOfEnforcedBound[bitPos] != lit_Undef);

    Lit litCounted = bitLitsOfCountedBound[bitPos];
    Lit litEnforced = bitLitsOfEnforcedBound[bitPos];

    // Define the variable saying that "the counted bit equals the enforced one"
    // (actually we only need the necessary conditions, not the sufficient ones)
    Lit litEqualBits = mkLit(SC->freshVariable() - 1, false);
    addTernaryClause(SC, litCounted, litEnforced, litEqualBits);
    addTernaryClause(SC, ~litCounted, ~litEnforced, litEqualBits);
    //addTernaryClause(SC, ~litEqualBits, litCounted, ~litEnforced);
    //addTernaryClause(SC, ~litEqualBits, ~litCounted, litEnforced);
    equalityLits.push_back(litEqualBits);
  }

  // Encode the actual "less than or equals" w.r.t. the bitwise assumable bound.
  for (int bitPos = 0; bitPos < bitLitsOfCountedBound.size(); ++bitPos) {
    // If the corresponding literal is undefined, we skip the bit position.
    if (bitLitsOfCountedBound[bitPos] == lit_Undef) {
      equalityLits.push_back(lit_Undef);
      continue;
    }

    vec<Lit> clauseToAdd;

    // "If the enforced bit at this position is 0 ..."
    clauseToAdd.push_back(bitLitsOfEnforcedBound[bitPos]);

    // For each larger (i.e., more significant) bit position:
    for (int otherBitPos = bitPos + 1; otherBitPos < bitLitsOfCountedBound.size(); ++otherBitPos) {
      // Undefined literal at the larger position?
      if (bitLitsOfCountedBound[otherBitPos] == lit_Undef) {
        continue;
      }
      // "And if the counted and the enforced bit at that larger position are the same ..."
      clauseToAdd.push_back(~equalityLits[otherBitPos]);
    }

    // "... then the counted bit at this position must be 0."
    clauseToAdd.push_back(~bitLitsOfCountedBound[bitPos]);

    addClause(SC, clauseToAdd);
  }
}

void Adder::encodeWithBitwiseAssumableBounds(Solver *SC, const vec<Lit> &lits, const vec<uint64_t> &coeffs) {

  _output.clear();

  // Maximum: sum of weights.
  uint64_t rhs = 0;
  for (auto coeff : coeffs) rhs += coeff;

  uint64_t nb = ld64(rhs); // number of bits
  Lit u = lit_Undef;

  // Build adder, with the output bits set in _output.
  for (int iBit = 0; iBit < nb; ++iBit) {
    _buckets.push_back(std::queue<Lit>());
    _output.push_back(u);
    for (int iVar = 0; iVar < lits.size(); ++iVar) {
      if (((((int64_t)1) << iBit) & coeffs[iVar]) != 0)
        _buckets.back().push(lits[iVar]);
    }
  }
  adderTree(SC, _buckets, _output);

  // Introduce new variables to later enforce the bits of the counted cost.
  _enforced_bound_lits.clear();
  for (int pos = 0; pos < _buckets.size(); ++pos)
    _enforced_bound_lits.push_back(mkLit(SC->freshVariable() - 1, false));

  // Encode the bound constraint relative to the new variables, which can be assumed by the user.
  encodeBitwiseAssumableBoundConstraint(SC, _output, _enforced_bound_lits);
  hasEncoding = true;
}

std::vector<Lit> Adder::enforceBoundBitwise(Solver *SC, uint64_t ub) {
  std::vector<Lit> assumptions;
  // Get bit representation of the bound
  std::vector<uint64_t> bits;
  numToBits(bits, _buckets.size(), ub);
  // Enforce the bits as individual assumptions
  for (int bitPos = 0; bitPos < _buckets.size(); ++bitPos) {
    if (bits[bitPos]) continue; // actually don't need to enforce the "1" bits
    assumptions.push_back(~_enforced_bound_lits[bitPos]);
  }
  return assumptions;
}

void Adder::updateInc(Solver *SC, uint64_t rhs, vec<Lit> &assumptions) {

  std::vector<uint64_t> kBits;
  numToBits(kBits, _buckets.size(), rhs);
  lessThanOrEqualInc(SC, _output, kBits, assumptions);
}

void Adder::update(Solver *SC, uint64_t rhs) {

  std::vector<uint64_t> kBits;
  numToBits(kBits, _buckets.size(), rhs);
  lessThanOrEqual(SC, _output, kBits);
}

uint64_t Adder::ld64(const uint64_t x) {
  return (sizeof(uint64_t) << 3) - __builtin_clzll(x);
  //   cout << "x " << x << endl;
  //   int ldretutn = 0;
  //   for (int i = 0; i < 63; ++i)
  //   {
  //     if ((x & (1 << i)) > 0)
  //     {
  //       cout << "ldretutn " << ldretutn << endl;
  //       ldretutn = i + 1;
  //     }
  //   }
  //
  //   return ldretutn;
}

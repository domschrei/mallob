#ifndef _optimizer_hpp_INCLUDED
#define _optimizer_hpp_INCLUDED

#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "cadical/src/cadical.hpp"
#include "util/logger.hpp"

#include <atomic>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <assert.h>
#include <limits.h>

using namespace std;

class OptimizingPropagator : public CaDiCaL::ExternalPropagator {

  long long int           obj_cur; // Objective value of the current solution
  long long int           obj_bsf; // Objective value of the best solution found so far
  vector< pair<long,int> > obj_lst; // Objective function as a list of pairs (coefficient, literal)
  long*                    obj_map; // Objective function as a map: lit -> coefficient

  int                     max_var; // Maximum variable in the formula
  long                    max_coe; // Maximum coefficient in the objective function

  int                       level; // Decision level
  signed char*            ass_map; // Literal assignment as a map: lit -> -1 (false) or 1 (true)
  vector<int>             ass_stk; // Literal assignment as a stack (literal 0 marks new decision level)
  vector<int>              clause; // Clause to be passed to SAT solver
  vector<int>             pending; // Literals that are pending to be propagated to SAT solver
  bool                  propagate; // True if new propagated literals have to be searched for.
  bool                    explain; // True if new explanation for a propagated literal must be computed.
  std::atomic_int         sol_cnt; // Counts the number of solutions found so far
  
  vector<int> best_solution; // best locally found solution
  std::atomic_llong obj_bsf_safe {LLONG_MAX}; // to let the outside know the internal progress
  std::atomic_llong best_cost_found_locally {LLONG_MAX}; // to decide whether a locally found solution is globally best
  std::atomic_llong best_cost_found_globally {LLONG_MAX}; // to share external improvements with the internal

  void set_true_in_assignment_map(int lit) {
    ass_map[ lit] =  1;
    ass_map[-lit] = -1;
    obj_cur += obj_map[lit];
  }

  void unset_in_assignment_map(int lit) {
    ass_map[lit] = ass_map[-lit] = 0;
    obj_cur -= obj_map[lit];
  }

  bool there_is_conflict();

public:

  OptimizingPropagator(const std::vector<std::pair<long, int>>& objective, int maxVar) :
    obj_cur(0),
    obj_bsf(LLONG_MAX),
    max_var(maxVar),
    max_coe( 0),
    level(0),
    propagate(true),
    explain(true),
    sol_cnt(0) {

    max_var = maxVar;

    int sz = 2*max_var + 1;

    ass_map = new signed char[sz];  memset (ass_map, 0,  sz);                  ass_map += max_var;
    obj_map = new        long[sz];  memset (obj_map, 0, (sz) * sizeof(long));  obj_map += max_var;

    for (const auto& [coe, lit] : objective) {
      assert(coe > 0);
      obj_lst.push_back({coe, lit});
      obj_map[lit] = coe;
      if (max_coe < coe) max_coe = coe;
    }

    // Sort in decreasing order of coefficient.
    sort(obj_lst.begin(), obj_lst.end(), greater<pair<long,int>>{});
    //for (auto& [coe, lit] : obj_lst) LOG(V2_INFO, "[prop] OBJ %ld * %i\n", coe, lit);
  }

  virtual ~OptimizingPropagator() {

    assert(max_var != -1);

    obj_map -= max_var;
    delete[] obj_map;

    ass_map -= max_var;
    delete[] ass_map;
  }

  int nb_solutions_found() const {return sol_cnt;}

  long long int best_objective_found_so_far() const { return obj_bsf_safe.load(std::memory_order_relaxed); }

  void update_best_found_objective_cost(long long cost) {
    best_cost_found_globally.store(cost, std::memory_order_relaxed);
  }

  bool has_best_solution() const {
    return best_cost_found_locally.load(std::memory_order_relaxed) == obj_bsf;
  }

  std::vector<int> get_solution() const {
    std::vector<int> result(max_var+1, 0);
    int solIdx = 0;
    for (int i = 1; i <= max_var; i++) {
      if (std::abs(best_solution[solIdx]) == i) {
        result[i] = best_solution[solIdx++];
      } else {
        result[i] = 0;
      }
    }
    return result;
  }

  void notify_assignment (const vector<int>& lits) override {

    // cerr << "Calling notify_assignment" << endl;

    for (int lit : lits) {
      set_true_in_assignment_map(lit);
      ass_stk.push_back(lit);
    }

    apply_best_solution_cost_from_outside();

    // cerr << "Called notify_assignment" << endl;
  };

  void notify_new_decision_level () override {

    // cerr << "Calling notify_new_decision_level" << endl;

    ++level;
    ass_stk.push_back(0);

    // cerr << "Called notify_new_decision_level" << endl;
  }

  int cb_add_external_clause_lit () override {

    // cerr << "Calling cb_add_external_clause_lit" << endl;

    int res;
    if (not clause.empty()) {
      res = clause.back();
      clause.pop_back();
    }
    else 
      res = 0;

    // cerr << "Called cb_add_external_clause_lit" << endl;

    return res;
  }

  bool cb_has_external_clause (bool& is_forgettable) override {

    // cerr << "Calling cb_has_external_clause" << endl;

    is_forgettable = false;
    bool res = there_is_conflict();
  
    // cerr << "Called cb_has_external_clause" << endl;

    return res;
  }
      
  int cb_decide () override {

    // cerr << "Calling cb_decide" << endl;

    int res = 0;
    /*for (const auto& [_, lit] : obj_lst)
      if (ass_map[lit] == 0) {
	res = -lit;
	break;
      }*/

    // cerr << "Called cb_decide" << endl;
    
    return res;
  }


  bool cb_check_found_model (const std::vector<int> &model) override;

  void notify_backtrack (size_t new_level) override;

  int cb_propagate () override;

  int cb_add_reason_clause_lit (int propagated_lit) override;

  void apply_best_solution_cost_from_outside() {
    // update solution cost from outside
    auto globalBest = best_cost_found_globally.load(std::memory_order_relaxed);
    if (MALLOB_LIKELY(globalBest >= obj_bsf)) return;
    LOG(V3_VERB, "[prop] update cost via external source: %lld\n", globalBest);
    obj_bsf = globalBest;
    obj_bsf_safe.store(obj_bsf, std::memory_order_relaxed);
    ++sol_cnt;
  }
};

#endif

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

  int nb_assigned = 0;
  bool lb_proven = false;

  void set_true_in_assignment_map(int lit) {
    //nb_assigned += ass_map[lit] == 0;
    ass_map[ lit] =  1;
    ass_map[-lit] = -1;
    obj_cur += obj_map[lit];
  }

  void unset_in_assignment_map(int lit) {
    ass_map[lit] = ass_map[-lit] = 0;
    obj_cur -= obj_map[lit];
    //--nb_assigned;
  }

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

  // negative if proven to be optimal
  long long int best_objective_found_so_far() const {
    auto bestCostFoundGlobally = best_cost_found_globally.load(std::memory_order_relaxed);
    if (bestCostFoundGlobally < 0) return bestCostFoundGlobally;
    return obj_bsf_safe.load(std::memory_order_relaxed);
  }

  void update_best_found_objective_cost(long long cost) {
    best_cost_found_globally.store(cost, std::memory_order_relaxed);
  }

  // Check whether this optimizer instance has access to a solution (model)
  // that matches its currently best known cost.
  bool has_best_solution() const {
    return best_cost_found_locally.load(std::memory_order_relaxed) == obj_bsf;
  }

  void set_lower_bound_proven() {
    update_best_found_objective_cost(-obj_bsf);
  }

  // Return a solution vector as Mallob expects it, i.e., beginning with a zero
  // and then containing the literal of variable i at position i.
  // TODO Is the model returned by cb_check_found_model only for the observed variables?
  // In that case we need to somehow gain access to the total model.
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

    // In principle we can do this anywhere, but it should be a place
    // that's not quite the absolute hottest path and that's still
    // called rather frequently.
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
    bool res = there_is_conflict() || lb_proven;
  
    // cerr << "Called cb_has_external_clause" << endl;

    return res;
  }
      
  int cb_decide () override {

    // cerr << "Calling cb_decide" << endl;

    //if (nb_assigned == obj_lst.size()) return 0;

    int res = 0;
    for (const auto& [_, lit] : obj_lst)
      if (ass_map[lit] == 0) {
        res = -lit;
        break;
      }

    // cerr << "Called cb_decide" << endl;
    
    return res;
  }

  // update solution cost set from the outside
  void apply_best_solution_cost_from_outside() {
    auto globalBest = best_cost_found_globally.load(std::memory_order_relaxed);
    lb_proven = lb_proven || (globalBest < 0);
    globalBest = std::abs(globalBest);
    if (MALLOB_LIKELY(globalBest >= obj_bsf)) return;
    LOG(V3_VERB, "[prop] update cost via external source: %lld\n", globalBest);
    obj_bsf = globalBest;
    obj_bsf_safe.store(obj_bsf, std::memory_order_relaxed);
    ++sol_cnt;
  }

  bool cb_check_found_model (const std::vector<int> &model) override {

    // cerr << "Calling cb_check_found_model" << endl;

    //cout << "c Found solution with objective " << obj_cur << endl;
    //LOG(V2_INFO, "[prop] found solution with cost %lu\n", obj_cur);

    ++sol_cnt;
    assert(obj_bsf > obj_cur);
    obj_bsf = obj_cur;
    obj_bsf_safe.store(obj_bsf, std::memory_order_relaxed);
    best_cost_found_locally.store(obj_bsf, std::memory_order_relaxed);

    best_solution = model;

    // Write model in DIMACS format onto a file whose name indicates the cost.
    //ofstream ost("modelWithCost-" + to_string(obj_bsf) + ".txt");
    //for (auto lit : model) 
    //  ost << "v " << lit << " " << endl;
    //ost << "v 0" << endl;
    
    // cerr << "Called cb_check_found_model" << endl;

    return false;
  }

  void notify_backtrack (size_t new_level) override {

    // cerr << "Calling notify_backtrack" << endl;

    if (level > new_level) { // Pending propagations may now have become unsound.
      pending.clear();
      propagate = true;
    }
    
    while (level > new_level) {
      int top_lit = ass_stk.back();
      ass_stk.pop_back();
      while (top_lit != 0) {
        unset_in_assignment_map(top_lit);
        top_lit = ass_stk.back();
        ass_stk.pop_back();
      }
      --level;
    }

    // cerr << "Called notify_backtrack" << endl;
  }

  int cb_propagate () override {

    // cerr << "Calling cb_propagate" << endl;

    if (propagate) {
      if (there_is_conflict()) {
        assert(not clause.empty());
        pending.push_back( clause.front() );
        explain = false;
      }
      else if (obj_bsf <= obj_cur + max_coe) {
        for (const auto& [coe, lit] : obj_lst)
          if (ass_map[lit] == 0) {
            if (obj_bsf <= obj_cur + coe) {
              pending.push_back(-lit);
            }
            else break;
          }
      }
      propagate = false;
    }

    int res;
    if (not pending.empty()) {
      res = pending.back();
      pending.pop_back();
    }
    else {
      res = 0;
      propagate = true;
    }

    // cerr << "Called cb_propagate" << endl;

    return res;
  };

  int cb_add_reason_clause_lit (int propagated_lit) override {

    // cerr << "Calling cb_add_reason_clause_lit(" << propagated_lit << ")" << endl;

    if (explain) {
      assert(clause.empty());
      clause.push_back(propagated_lit);
      int coe = obj_map[-propagated_lit];
      long long int obj = 0;
      for (int lit : ass_stk) {
        assert(lit != propagated_lit);
        if (lit != 0 and lit != -propagated_lit and obj_map[lit] > 0) {
          obj += obj_map[lit];
          clause.push_back(-lit);
          if (obj_bsf <= obj + coe) break;
        }
      }
      assert(obj_bsf <= obj + coe);
      explain = false;
    }

    int res;
    if (not clause.empty()) {
      res = clause.back();
      clause.pop_back();
    }
    else {
      res = 0;
      explain = true;
    }

    // cerr << "Called cb_add_reason_clause_lit" << endl;

    return res;
  };

  bool there_is_conflict() {

    assert(clause.empty());
    
    if (obj_bsf <= obj_cur) {
      long long int obj = 0;
      for (const auto& [coe, lit] : obj_lst)
        if (ass_map[lit] > 0) {
          clause.push_back(-lit);
          obj += coe;
          if (obj_bsf <= obj) break;
        }
      return true;
    }
    return false;
  }
};

#endif

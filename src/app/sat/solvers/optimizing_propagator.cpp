#include <atomic>
#include <fstream>
#include <string.h>
#include "optimizing_propagator.hpp"
#include "util/logger.hpp"

bool OptimizingPropagator::cb_check_found_model (const std::vector<int> &model) {

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


void OptimizingPropagator::notify_backtrack (size_t new_level) {

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


int OptimizingPropagator::cb_propagate () {

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


int OptimizingPropagator::cb_add_reason_clause_lit (int propagated_lit) {

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


bool OptimizingPropagator::there_is_conflict() {

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
  else
    return false;
}


#include "minisat.hpp"
#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "minisat/core/Solver.h"
#include "minisat/core/SolverTypes.h"
#include "minisat/mtl/Vec.h"

// Taken from IPASIR implementation of Minisat
/* Copyright (C) 2011 - 2014, Armin Biere, Johannes Kepler University, Linz */
/* Copyright (C) 2014, Mathias Preiner, Johannes Kepler University, Linz */
class IPAsirMiniSAT : public Minisat::Solver {
  Minisat::vec<Minisat::Lit> assumptions, clause;
  int szfmap; unsigned char * fmap; bool nomodel;
  unsigned long long calls;
  void reset () { if (fmap) delete [] fmap, fmap = 0, szfmap = 0; }
  Minisat::Lit import (int32_t lit) {
    while (abs (lit) > nVars ()) (void) newVar ();
    return Minisat::mkLit (Minisat::Var (abs (lit) - 1), (lit < 0));
  }
  void ana () {
    fmap = new unsigned char [szfmap = nVars ()];
    memset (fmap, 0, szfmap);
    for (int i = 0; i < conflict.size (); i++) {
      int tmp = var (conflict[i]);
      assert (0 <= tmp && tmp < szfmap);
      fmap[tmp] = 1;
    }
  }
  double ps (double s, double t) { return t ? s/t : 0; }
public:
  IPAsirMiniSAT () : szfmap (0), fmap (0), nomodel (false), calls (0) {
    // MiniSAT by default produces non standard conforming messages.
    // So either we have to set this to '0' or patch the sources.
    verbosity = 1;
  }
  ~IPAsirMiniSAT () { reset (); }
  void add (int32_t lit) {
    reset ();
    nomodel = true;
    if (lit) clause.push (import (lit));
    else addClause (clause), clause.clear ();
  }
  void assume (int32_t lit) {
    reset ();
    nomodel = true;
    assumptions.push (import (lit));
  }
  void setVariablePhase (int32_t lit, bool flag) {
    setPolarity(var (import (lit)), flag ? Minisat::l_True : Minisat::l_False);
  }
  int solve () {
    calls++;
    reset ();
    Minisat::lbool res = solveLimited (assumptions);
    assumptions.clear ();
    nomodel = (res != Minisat::l_True);
    return (res == Minisat::l_Undef) ? 0 : (res == Minisat::l_True ? 10 : 20);
  }
  int val (int32_t lit) {
    if (nomodel) return 0;
    Minisat::lbool res = modelValue (import (lit));
    return (res == Minisat::l_True) ? lit : -lit;
  }
  bool failed (int32_t lit) {
    if (!fmap) ana ();
    int tmp = var (import (lit));
    assert (0 <= tmp && tmp < nVars ());
    return fmap[tmp] != 0;
  }
};



int cbTerminate(void * state) {
    MiniSat* ms = (MiniSat*) state;
    return ms->interrupt;
}

MiniSat::MiniSat(const SolverSetup& setup) : PortfolioSolverInterface(setup) {
    _solver = new IPAsirMiniSAT();
    _solver->setTermCallback(this, cbTerminate);
}
MiniSat::~MiniSat() {
    if (_solver) delete _solver;
}

// Get the number of variables of the formula
int MiniSat::getVariablesCount() {
    return _solver->nVars();
}

// Get a variable suitable for search splitting
int MiniSat::getSplittingVariable() {
    return 0;
}

// Set initial phase for a given variable
// Used only for diversification of the portfolio
void MiniSat::setPhase(const int var, const bool phase) {
    _solver->setVariablePhase(var, phase);
}

// Solve the formula with a given set of assumptions
SatResult MiniSat::solve(size_t numAssumptions, const int* assumptions) {
    _assumptions = std::vector<int>(assumptions, assumptions+numAssumptions);
    for (int l : _assumptions) _solver->assume(l);
    return SatResult(_solver->solve());
}

// Get a solution vector containing lit or -lit for each lit in the model
std::vector<int> MiniSat::getSolution() {
    std::vector<int> sol(1, 0);
    for (int i = 1; i <= _solver->nVars(); i++) sol.push_back(_solver->val(i));
    return sol;
}

// Get a set of failed assumptions
std::set<int> MiniSat::getFailedAssumptions() {
    std::set<int> failed;
    for (int lit : _assumptions) if (_solver->failed(lit)) failed.insert(lit);
    return failed;
}

// Add a permanent literal to the formula (zero for clause separator)
void MiniSat::addLiteral(int lit) {
    _solver->add(lit);
}

// Set a function that should be called for each learned clause
void MiniSat::setLearnedClauseCallback(const LearnedClauseCallback& callback) {}
//void ipasir_set_learn (void * s, void * state, int max_length, void (*learn)(void * state, int32_t * clause)) { import(s)->setLearnCallback(state, max_length, learn); }

// Set a function that can be called to probe whether a clause of specified length
// may be eligible for export. (It might still be rejected upon export.)
void MiniSat::setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& callback) {}

// Get solver statistics
void MiniSat::writeStatistics(SolverStatistics& stats) {
    stats.conflicts = _solver->conflicts;
    stats.propagations = _solver->propagations;
    stats.decisions = _solver->decisions;
    stats.restarts = _solver->starts;
    stats.producedClauses = _solver->num_learnts;
}

// Diversify your parameters (seeds, heuristics, etc.) according to the seed
// and the individual diversification index given by getDiversificationIndex().
void MiniSat::diversify(int seed) {
    _solver->random_seed = seed;
}

// How many "true" different diversifications do you have?
// May be used to decide when to apply additional diversifications.
int MiniSat::getNumOriginalDiversifications() {
    return 1;
}

bool MiniSat::supportsIncrementalSat() {
    return true;
}
bool MiniSat::exportsConditionalClauses() {
    return false;
}

void MiniSat::cleanUp() {}

// Interrupt the SAT solving, solving cannot continue until interrupt is unset.
void MiniSat::setSolverInterrupt() {interrupt = true;}

// Resume SAT solving after it was interrupted.
void MiniSat::unsetSolverInterrupt() {interrupt = false;}

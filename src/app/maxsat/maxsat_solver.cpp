
#include "maxsat_solver.hpp"

void maxsat_collect_clause(int lit, void* solver) {
    ((MaxSatSolver*)solver)->appendLiteral(lit);
}
void maxsat_collect_assumption(int lit, void* solver) {
    ((MaxSatSolver*)solver)->appendAssumption(lit);
}

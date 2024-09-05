
#include "maxsat_search_procedure.hpp"

int MaxSatSearchProcedure::_running_stream_id = 1;

void maxsat_collect_clause(int lit, void* search) {
    ((MaxSatSearchProcedure*)search)->appendLiteral(lit);
}
void maxsat_collect_assumption(int lit, void* search) {
    ((MaxSatSearchProcedure*)search)->appendAssumption(lit);
}

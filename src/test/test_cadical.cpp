#include <cassert>

#include "app/sat/hordesat/utilities/default_logging_interface.hpp"
#include "util/sat_reader.hpp"

#include "app/sat/hordesat/solvers/cadical.hpp"

void testSolveSimpleFormula() {
    DefaultLoggingInterface logger;
    Cadical cadical = Cadical(logger, 0, 0, "TESTJOB");

    SatReader r("/home/maxi/gates/6s33.cnf");
    auto formula = r.read();

    // Read formula
    for (int lit : *formula)
        cadical.addLiteral(lit);

    auto res = cadical.solve({});

    assert(res == SatResult::UNSAT);
}

int main() {
    testSolveSimpleFormula();

    return 0;
}

#include <cassert>

#include <iostream>

#include <chrono>
#include <thread>

#include "app/sat/hordesat/utilities/default_logging_interface.hpp"
#include "util/sat_reader.hpp"

#include "app/sat/hordesat/solvers/cadical.hpp"

void testSolveSimpleFormula() {
    printf("testSolveSimpleFormula\n");

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

void terminateSolver(PortfolioSolverInterface *solver) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    solver->interrupt();
}

void testTermination() {
    printf("testTermination\n");

    DefaultLoggingInterface logger;
    Cadical cadical = Cadical(logger, 0, 0, "TESTJOB");

    SatReader r("/home/maxi/gates/vmpc_32.renamed-as.sat05-1919.cnf");
    auto formula = r.read();

    // Read formula
    for (int lit : *formula)
        cadical.addLiteral(lit);

    // Start delayed terminate
    std::thread terminateThread(terminateSolver, &cadical);

    auto res = cadical.solve({});

    terminateThread.join();

    assert(res == SatResult::UNKNOWN);
}

void unsuspendSolver(PortfolioSolverInterface *solver) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    solver->resume();
}

void testSuspend() {
    printf("testSuspend\n");

    DefaultLoggingInterface logger;
    Cadical cadical = Cadical(logger, 0, 0, "TESTJOB");

    SatReader r("/home/maxi/gates/6s33.cnf");
    auto formula = r.read();

    // Read formula
    for (int lit : *formula)
        cadical.addLiteral(lit);

    // Start delayed unsuspend
    std::thread unsuspendThread(unsuspendSolver, &cadical);

    cadical.suspend();

    auto res = cadical.solve({});

    unsuspendThread.join();

    assert(res == SatResult::UNSAT);
}

int main() {
    testSolveSimpleFormula();

    testTermination();

    testSuspend();

    return 0;
}

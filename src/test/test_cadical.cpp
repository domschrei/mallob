#include "util/assert.hpp"

#include <iostream>

#include <chrono>
#include <thread>

#include "app/sat/hordesat/utilities/default_logging_interface.hpp"
#include "util/sat_reader.hpp"

#include "app/sat/hordesat/solvers/cadical.hpp"

void testSolveSimpleFormula() {
    printf("testSolveSimpleFormula\n");

    DefaultConsole logger;
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

    DefaultConsole logger;
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

    DefaultConsole logger;
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

struct Callback : public LearnedClauseCallback {
    // Default glueLimit
    const int glueLimit = 2;

    void processClause(vector<int>& cls, int solverId) override {
        assert(cls.size() <= glueLimit + 1);

        if (cls.size() > 1)
            assert(cls.at(0) == cls.size());
    }
};

void testExportClauses() {
    printf("testExportClauses\n");

    DefaultConsole logger;
    Cadical cadical = Cadical(logger, 0, 0, "TESTJOB");

    SatReader r("/home/maxi/gates/6s33.cnf");
    auto formula = r.read();

    // Read formula
    for (int lit : *formula)
        cadical.addLiteral(lit);

    Callback callback;

    cadical.setLearnedClauseCallback(&callback);

    auto res = cadical.solve({});

    assert(res == SatResult::UNSAT);
}

void testAssume() {
    printf("testAssume\n");

    DefaultConsole logger;
    Cadical cadical = Cadical(logger, 0, 0, "TESTJOB");

    SatReader r("/home/maxi/gates/vmpc_32.renamed-as.sat05-1919.cnf");
    auto formula = r.read();

    // Read formula
    for (int lit : *formula)
        cadical.addLiteral(lit);

    auto res = cadical.solve({-179, -980, -565, 105, 783});

    assert(res == SatResult::SAT);
}

void testImportUnits() {
    printf("testImportUnits\n");

    DefaultConsole logger;
    Cadical cadical = Cadical(logger, 0, 0, "TESTJOB");

    SatReader r("/home/maxi/gates/vmpc_32.renamed-as.sat05-1919.cnf");
    auto formula = r.read();

    // Read formula
    for (int lit : *formula)
        cadical.addLiteral(lit);

    std::array<int, 1> clause1 = {-179};
    std::array<int, 1> clause2 = {-980};
    std::array<int, 1> clause3 = {-565};
    std::array<int, 1> clause4 = {105};
    std::array<int, 1> clause5 = {783};
    cadical.addLearnedClause(clause1.data(), 1);
    cadical.addLearnedClause(clause2.data(), 1);
    cadical.addLearnedClause(clause3.data(), 1);
    cadical.addLearnedClause(clause4.data(), 1);
    cadical.addLearnedClause(clause5.data(), 1);

    auto res = cadical.solve({});

    assert(res == SatResult::SAT);
}

void testImportFailingClause() {
    printf("testImportFailingClause\n");

    DefaultConsole logger;
    Cadical cadical = Cadical(logger, 0, 0, "TESTJOB");

    SatReader r("/home/maxi/gates/vmpc_32.renamed-as.sat05-1919.cnf");
    auto formula = r.read();

    // Read formula
    for (int lit : *formula)
        cadical.addLiteral(lit);

    std::array<int, 3> clause = {3, 179, -179};
    cadical.addLearnedClause(clause.data(), 3);

    auto res = cadical.solve({});

    assert(res == SatResult::UNSAT);
}

int main() {
    testSolveSimpleFormula();

    testAssume();

    testTermination();

    testSuspend();

    testExportClauses();

    testImportUnits();

    // Takes to long for some reason
    // FIXME
    // testImportFailingClause();

    return 0;
}

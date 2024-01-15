
#include "app/sat/data/clause.hpp"
#include "app/sat/proof/lrat_checker.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/random.hpp"
#include "cadical/src/signature.hpp"
#include "util/sys/timer.hpp"
#include <iostream>

int main(int argc, char** argv) {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);

    Parameters params;
    params.init(argc, argv);

    std::vector<int> orig {
        1, -2, 0,
        2, -4, 0,
        1, 2, 4, 0,
        -1, -3, 0,
        1, -3, 0,
        -1, 3, 0,
        1, 3, -4, 0,
        1, 3, 4, 0
    };

    LratChecker chk(4);
    bool ok;
    ok = chk.loadOriginalClauses(orig.data(), orig.size()); assert(ok);
    ok = chk.addClause(9, {-3}, {5, 4}); assert(ok);
    ok = chk.addClause(10, {1, 2}, {3, 2}); assert(ok);
    ok = chk.addClause(11, {-1}, {6, 9}); assert(ok);
    ok = chk.deleteClause({9}); assert(ok);

    ok = chk.addClause(12, {2, 3, -4}, {11}); assert(!ok); // INCORRECT - incomplete hints
    ok = chk.addClause(12, {2, 3, -4}, {7}); assert(!ok); // INCORRECT - incomplete hints
    ok = chk.addClause(12, {2, 3, -4}, {6}); assert(!ok); // INCORRECT - wrong hint
    ok = chk.addClause(12, {2, 3, -4}, {7, 11, 11}); assert(!ok); // INCORRECT - non-final clause becomes empty
    ok = chk.addClause(12, {2, 3, -4}, {9}); assert(!ok); // INCORRECT - deleted clause

    ok = chk.addClause(12, {2, 3, -4}, {7, 11}); assert(ok);
    ok = chk.addClause(13, {1, 2, 3}, {8, 12}); assert(ok);
    ok = chk.addClause(14, {}, {11, 10, 1}); assert(ok);
    ok = chk.validateUnsat(); assert(ok);
}

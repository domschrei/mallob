
#include <assert.h>
#include <cstdio>
#include <stdlib.h>
#include <cstdint>
#include <vector>

#include "app/sat/proof/trusted/lrat_checker.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/random.hpp"
#include "util/sys/timer.hpp"

bool addCls(LratChecker& chk, uint64_t id, const std::vector<int>& lits, const std::vector<uint64_t>& hints) {
    return chk.addClause(id, lits.data(), lits.size(), hints.data(), hints.size());
}
bool delCls(LratChecker& chk, const std::vector<uint64_t>& ids) {
    return chk.deleteClause(ids.data(), ids.size());
}

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
    ok = addCls(chk, 9, {-3}, {5, 4}); assert(ok);
    ok = addCls(chk, 10, {1, 2}, {3, 2}); assert(ok);
    ok = addCls(chk, 11, {-1}, {6, 9}); assert(ok);
    ok = delCls(chk, {9}); assert(ok);

    ok = addCls(chk, 12, {2, 3, -4}, {11}); assert(!ok); // INCORRECT - incomplete hints
    ok = addCls(chk, 12, {2, 3, -4}, {7}); assert(!ok); // INCORRECT - incomplete hints
    ok = addCls(chk, 12, {2, 3, -4}, {6}); assert(!ok); // INCORRECT - wrong hint
    ok = addCls(chk, 12, {2, 3, -4}, {7, 11, 11}); assert(!ok); // INCORRECT - non-final clause becomes empty
    ok = addCls(chk, 12, {2, 3, -4}, {9}); assert(!ok); // INCORRECT - deleted clause

    ok = addCls(chk, 12, {2, 3, -4}, {7, 11}); assert(ok);
    ok = addCls(chk, 13, {1, 2, 3}, {8, 12}); assert(ok);
    ok = addCls(chk, 14, {}, {11, 10, 1}); assert(ok);
    ok = chk.validateUnsat(); assert(ok);

    printf("All ok.\n");
}

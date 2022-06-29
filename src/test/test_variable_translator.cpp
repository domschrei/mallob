
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>
#include <thread>
#include <set>

#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "app/sat/execution/variable_translator.hpp"

void test() {
    LOG(V2_INFO, "Testing variable translator ...\n");
    
    VariableTranslator vt;
    int maxVar = 10;
    for (int var = 1; var <= maxVar; var++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            assert(vt.getTldLit(sign*var) == sign*var);
            assert(vt.getOrigLitOrZero(sign*var) == sign*var);
        }
    }

    std::vector<int> extraVars;

    vt.addExtraVariable(maxVar);
    extraVars.push_back(vt.getTldLit(maxVar)+1);
    maxVar = 20;

    LOG(V2_INFO, "Extra variables: ");
    for (int lit : vt.getExtraVariables()) LOG_OMIT_PREFIX(V2_INFO, "%i ", lit);
    LOG_OMIT_PREFIX(V2_INFO, "\n");
    assert(extraVars == vt.getExtraVariables());

    assert(vt.getTldLit(1) == 1);
    assert(vt.getTldLit(2) == 2);
    assert(vt.getTldLit(-3) == -3);
    assert(vt.getTldLit(9) == 9);
    assert(vt.getTldLit(-10) == -10);
    assert(vt.getTldLit(11) == 12);
    assert(vt.getTldLit(-12) == -13);

    assert(vt.getOrigLitOrZero(1) == 1);
    assert(vt.getOrigLitOrZero(2) == 2);
    assert(vt.getOrigLitOrZero(-3) == -3);
    assert(vt.getOrigLitOrZero(9) == 9);
    assert(vt.getOrigLitOrZero(-10) == -10);
    assert(vt.getOrigLitOrZero(11) == 0);
    assert(vt.getOrigLitOrZero(-12) == -11);

    vt.addExtraVariable(maxVar);
    extraVars.push_back(vt.getTldLit(maxVar)+1);
    maxVar = 30;

    LOG(V2_INFO, "Extra variables: ");
    for (int lit : vt.getExtraVariables()) LOG_OMIT_PREFIX(V2_INFO, "%i ", lit);
    LOG_OMIT_PREFIX(V2_INFO, "\n");
    assert(extraVars == vt.getExtraVariables());
    for (int extraVar : extraVars) assert(vt.getOrigLitOrZero(extraVar) == 0);
    // extra var 11
    assert(vt.getTldLit(10) == 10);
    assert(vt.getTldLit(11) == 12);
    assert(vt.getOrigLitOrZero(10) == 10);
    assert(vt.getOrigLitOrZero(12) == 11);
    // extra var 22
    assert(vt.getTldLit(20) == 21);
    assert(vt.getTldLit(21) == 23);
    assert(vt.getOrigLitOrZero(21) == 20);
    assert(vt.getOrigLitOrZero(23) == 21);

    vt.addExtraVariable(maxVar);
    extraVars.push_back(vt.getExtraVariables().back());
    vt.addExtraVariable(maxVar);
    extraVars.push_back(vt.getExtraVariables().back());
    maxVar = 40;

    LOG(V2_INFO, "Extra variables: ");
    for (int lit : vt.getExtraVariables()) LOG_OMIT_PREFIX(V2_INFO, "%i ", lit);
    LOG_OMIT_PREFIX(V2_INFO, "\n");
    assert(extraVars == vt.getExtraVariables());
    for (int extraVar : extraVars) assert(vt.getOrigLitOrZero(extraVar) == 0);
    // extra vars 33, 34
    assert(vt.getTldLit(30) == 32);
    assert(vt.getTldLit(31) == 35);
    assert(vt.getOrigLitOrZero(32) == 30);
    assert(vt.getOrigLitOrZero(35) == 31);
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    test();
}


#include "util/assert.hpp"
#include <stdio.h>
#include <memory>
#include <string>
#include <vector>

#include "app/sat/data/portfolio_sequence.hpp"

int main() {
    PortfolioSequence seq;

    assert(!seq.parse("*"));

    assert(seq.parse(""));
    printf("%s\n", seq.toStr().c_str());
    assert(seq.prefix.empty());
    assert(seq.cycle.empty());

    assert(seq.parse("k"));
    printf("%s\n", seq.toStr().c_str());
    assert(seq.prefix.empty());
    assert(seq.cycle.size() == 1);
    assert(seq.cycle[0].baseSolver == PortfolioSequence::KISSAT);

    assert(seq.parse("kcl"));
    printf("%s\n", seq.toStr().c_str());
    assert(seq.prefix.empty());
    assert(seq.cycle.size()==3);

    assert(seq.parse("k+"));
    assert(seq.prefix.empty());
    assert(seq.cycle.size()==1);
    assert(seq.cycle[0].flavour == PortfolioSequence::SAT);

    assert(seq.parse("(k){4}"));
    assert(seq.prefix.empty());
    assert(seq.cycle.size()==4);
    assert(seq.cycle[2].baseSolver == PortfolioSequence::KISSAT);

    assert(seq.parse("(kkc){3}"));
    assert(seq.prefix.empty());
    assert(seq.cycle.size()==9);
    assert(seq.cycle[2].baseSolver == PortfolioSequence::CADICAL);
    assert(seq.cycle[7].baseSolver == PortfolioSequence::KISSAT);
    assert(seq.cycle[8].baseSolver == PortfolioSequence::CADICAL);

    assert(seq.parse("(k+){5}(kcl)*"));
    assert(seq.prefix.size()==5);
    assert(seq.cycle.size()==3);

    assert(seq.parse("k+c-(l-l+){2}"));
    printf("%s\n", seq.toStr().c_str());
    assert(seq.prefix.size()==0);
    assert(seq.cycle.size()==6);

    assert(seq.parse("(kc(ll){2}){3}"));
    printf("%s\n", seq.toStr().c_str());
    assert(seq.prefix.empty());
    assert(seq.cycle.size() == 18);

    assert(!seq.parse("k*c*l*"));

    assert(seq.parse("(k+){4}c!*"));
    printf("%s\n", seq.toStr().c_str());
    
    assert(seq.parse("(k+(c!){23}){4}c!*"));
    printf("%s\n", seq.toStr().c_str());
    assert(seq.prefix.size()==4*24);
    assert(seq.cycle.size()==1);
}

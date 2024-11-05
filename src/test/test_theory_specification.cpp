
#include "app/sat/data/theories/theory_specification.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"
#include "util/assert.hpp"

void test() {
    for (auto rule : {
        "min 1",             
        "min (4)",             
        "max -420000001",         
        "max 420009991",         
        "max 1+2",    
        "max (3-42)+5",         
        "max 3-(42+5)",       
        "max .72",             
        "max .12+-.34",          
        "max !12*7+3",          
        "max .12*7^3",          
        "max (#12*7)^3",       
        "max (3/4)%-(.34+.35)",
        "inv 1 < 2",
        "inv 3*.76+.77/4 >= !45+!46+!47",
        "inv .1*.2*.3*.4*.5*.6*.7*.8 < 4"
    }) {
        auto theory = TheorySpecification(rule);
        LOG(V2_INFO, "THEORY_SPEC \"%s\" -> \"%s\"\n", rule, theory.getRuleset().at(0).toStr().c_str());

        auto packed = theory.serialize();
        LOG(V2_INFO, "  - serialized: size %lu\n", packed.size());
        TheorySpecification reconstructed;
        reconstructed.deserialize(packed);
        LOG(V2_INFO, "  - deserialized+serialized: size %lu\n", reconstructed.serialize().size());
        assert(theory.getRuleset().size() == reconstructed.getRuleset().size());
        for (size_t i = 0; i < theory.getRuleset().size(); i++) {
            LOG(V2_INFO, "  - %s vs. %s\n", theory.getRuleset()[i].toStr().c_str(), reconstructed.getRuleset()[i].toStr().c_str());
            assert(theory.getRuleset()[i].toStr() == reconstructed.getRuleset()[i].toStr());
        }
    }

    std::string longRule = "min ";
    for (size_t i = 1; i <= 100'000; i++) longRule += "3*#" + std::to_string(i) + " + ";
    longRule = longRule.substr(0, longRule.size()-2);

    LOG(V2_INFO, "begin parse long theory\n");
    auto theory = TheorySpecification(longRule.c_str());
    LOG(V2_INFO, "end parse long theory\n");

    //LOG(V2_INFO, "THEORY_SPEC \"%s\" -> \"%s\"\n", longRule.c_str(),
    //    theory.getRuleset().at(0).toStr().c_str());

    struct Evaluator {
        long operator[](long idx) const {
            return idx % 3 == 0 ? 1 : 0;
        }
    } eval;
    long val = theory.getRuleset()[0].term1.evaluate(eval);
    LOG(V2_INFO, "value = %ld\n", val);

    LOG(V2_INFO, "  - string: size %lu\n", theory.getRuleset()[0].toStr().size());
    auto packed = theory.serialize();
    LOG(V2_INFO, "  - serialized: size %lu\n", packed.size());
    TheorySpecification reconstructed;
    reconstructed.deserialize(packed);
    LOG(V2_INFO, "  - deserialized+serialized: size %lu\n", reconstructed.serialize().size());
    assert(theory.getRuleset().size() == reconstructed.getRuleset().size());
    for (size_t i = 0; i < theory.getRuleset().size(); i++) {
        assert(theory.getRuleset()[i].toStr() == reconstructed.getRuleset()[i].toStr());
    }
}

int main() {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);
    ProcessWideThreadPool::init(1);

    test();
}

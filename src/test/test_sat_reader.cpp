
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>

#include "util/random.hpp"
#include "app/sat/parse/sat_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

int main() {

    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);

    auto files = {"Steiner-9-5-bce.cnf.xz", "uum12.smt2.cnf.xz", 
        "LED_round_29-32_faultAt_29_fault_injections_5_seed_1579630418.cnf.xz", "SAT_dat.k80.cnf.xz", "Timetable_C_497_E_62_Cl_33_S_30.cnf.xz", 
        "course0.2_2018_3-sc2018.cnf.xz", "sv-comp19_prop-reachsafety.queue_longer_false-unreach-call.i-witness.cnf.xz"};

    for (const auto& file : files) {
        auto f = std::string("instances/") + file;
        LOG(V2_INFO, "Reading test CNF %s ...\n", f.c_str());
        float time = Timer::elapsedSeconds();
        SatReader r(f);
        JobDescription d;
        bool success = r.read(d);
        assert(success);
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
        assert(d.getNumFormulaLiterals() > 0);

        LOG(V2_INFO, "Only decompressing CNF %s for comparison ...\n", f.c_str());
        float time2 = Timer::elapsedSeconds();
        auto cmd = "xz -c -d " + f + " > /tmp/tmpfile";
        int retval = system(cmd.c_str());
        time2 = Timer::elapsedSeconds() - time2;
        LOG(V2_INFO, " - done, took %.3fs\n", time2);
        assert(retval == 0);

        LOG(V2_INFO, " -- difference: %.3fs\n", time - time2);
    }
}
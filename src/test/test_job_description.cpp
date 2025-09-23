
#include <assert.h>
#include <stdlib.h>
#include <string>
#include <initializer_list>
#include <memory>

#include "util/random.hpp"
#include "app/sat/parse/sat_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "app/app_registry.hpp"
#include "data/job_description.hpp"
#include "util/params.hpp"

void testSatInstances(Parameters& params) {

    auto files = {"Steiner-9-5-bce.cnf.xz", "uum12.smt2.cnf.xz", 
        "LED_round_29-32_faultAt_29_fault_injections_5_seed_1579630418.cnf.xz", "SAT_dat.k80.cnf.xz", "Timetable_C_497_E_62_Cl_33_S_30.cnf.xz", 
        "course0.2_2018_3-sc2018.cnf.xz", "sv-comp19_prop-reachsafety.queue_longer_false-unreach-call.i-witness.cnf.xz"};

    for (const auto& file : files) {
        auto f = std::string("instances/") + file;
        LOG(V2_INFO, "Reading test CNF %s ...\n", f.c_str());
        float time = Timer::elapsedSeconds();
        SatReader r(params, f);
        JobDescription d;
        bool success = r.read(d);
        assert(success);
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);

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

void testIncrementalExample(Parameters& params) {

    JobDescription desc(1, 1, app_registry::getAppId("SAT"), true);
    desc.setIncremental(true);
    std::string f = "instances/incremental/entertainment08-0.cnf";
    SatReader r(params, f);
    r.read(desc);

    auto exported = desc.getSerialization(0);

    JobDescription imported(1, 1, app_registry::getAppId("SAT"), true);
    imported.setIncremental(true);
    imported.deserialize(exported);
    assert(desc.getFormulaPayloadSize(0) == imported.getFormulaPayloadSize(0));
    for (size_t i = 0; i < desc.getFormulaPayloadSize(0); i++)
        assert(desc.getFormulaPayload(0)[i] == imported.getFormulaPayload(0)[i]);

    f = "instances/incremental/entertainment08-1.cnf";
    SatReader r2(params, f);
    JobDescription update(1, 1, app_registry::getAppId("SAT"), true);
    update.setIncremental(true);
    update.setRevision(1);
    r2.read(update);
    exported = update.getSerialization(1);
    JobDescription imported1(1, 1, app_registry::getAppId("SAT"), true);
    imported1.setIncremental(true);
    imported1.deserialize(exported);
}

int main(int argc, char *argv[]) {

    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);

    Parameters params;
    params.init(argc, argv);

    testSatInstances(params);
    testIncrementalExample(params);
}
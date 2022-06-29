
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>

#include "util/random.hpp"
#include "util/sat_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

void testSatInstances() {

    auto files = {"Steiner-9-5-bce.cnf.xz", "uum12.smt2.cnf.xz", 
        "LED_round_29-32_faultAt_29_fault_injections_5_seed_1579630418.cnf.xz", "SAT_dat.k80.cnf.xz", "Timetable_C_497_E_62_Cl_33_S_30.cnf.xz", 
        "course0.2_2018_3-sc2018.cnf.xz", "sv-comp19_prop-reachsafety.queue_longer_false-unreach-call.i-witness.cnf.xz"};

    for (const auto& file : files) {
        auto f = std::string("instances/") + file;
        LOG(V2_INFO, "Reading test CNF %s ...\n", f.c_str());
        float time = Timer::elapsedSeconds();
        SatReader r(f, SatReader::ContentMode::ASCII);
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

void testIncrementalExample() {

    JobDescription desc(1, 1, JobDescription::Application::INCREMENTAL_SAT, true);
    std::string f = "instances/incremental/entertainment08-0.cnf";
    SatReader r(f, SatReader::ContentMode::ASCII);
    r.read(desc);
    LOG(V2_INFO, "Base: %i lits, %i assumptions\n", desc.getNumFormulaLiterals(), desc.getNumAssumptionLiterals());
    assert(desc.getNumFormulaLiterals() == 6);
    assert(desc.getNumAssumptionLiterals() == 1);

    auto exported = desc.getSerialization(0);

    JobDescription imported(1, 1, JobDescription::Application::INCREMENTAL_SAT, true);
    imported.deserialize(exported);
    assert(imported.getNumFormulaLiterals() == 6);
    assert(imported.getNumAssumptionLiterals() == 1);
    assert(desc.getFormulaPayloadSize(0) == imported.getFormulaPayloadSize(0));
    for (size_t i = 0; i < desc.getFormulaPayloadSize(0); i++) {
        assert(desc.getFormulaPayload(0)[i] == imported.getFormulaPayload(0)[i]);
    }
    assert(desc.getAssumptionsSize(0) == imported.getAssumptionsSize(0));
    for (size_t i = 0; i < desc.getAssumptionsSize(0); i++) {
        LOG(V2_INFO, "Asmpt %i\n", desc.getAssumptionsPayload(0)[i]);
        assert(desc.getAssumptionsPayload(0)[i] == imported.getAssumptionsPayload(0)[i]);
    }

    f = "instances/incremental/entertainment08-1.cnf";
    r = SatReader(f, SatReader::ContentMode::ASCII);
    JobDescription update(1, 1, JobDescription::Application::INCREMENTAL_SAT, true);
    update.setRevision(1);
    r.read(update);
    LOG(V2_INFO, "Update: %i lits, %i assumptions\n", update.getNumFormulaLiterals(), update.getNumAssumptionLiterals());
    exported = update.getSerialization(1);
    JobDescription imported1(1, 1, JobDescription::Application::INCREMENTAL_SAT, true);
    imported1.deserialize(exported);
    
    assert(imported1.getNumAssumptionLiterals() == update.getNumAssumptionLiterals());
    assert(update.getAssumptionsSize(1) == update.getNumAssumptionLiterals());
    for (size_t i = 0; i < update.getAssumptionsSize(1); i++) {
        LOG(V2_INFO, "Asmpt %i\n", update.getAssumptionsPayload(1)[i]);
        assert(update.getAssumptionsPayload(1)[i] == imported1.getAssumptionsPayload(1)[i]);
    }
}

int main() {

    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);

    testSatInstances();
    testIncrementalExample();
}
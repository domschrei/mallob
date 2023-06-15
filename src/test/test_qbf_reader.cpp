
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>

#include "util/random.hpp"
#include "app/qbf/parse/qbf_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

int main(int argc, char *argv[]) {

    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V6_DEBGV);

    Parameters params;
    params.init(argc, argv);

    auto files = {"../r3unsat_300.cnf", "microtest01.qdimacs", "hein_04_3x3_05.pg.qdimacs"};

    for (const auto& file : files) {
        auto f = std::string("instances/qbf/") + file;
        LOG(V2_INFO, "Reading test QBF %s ...\n", f.c_str());
        float time = Timer::elapsedSeconds();
        QbfReader r(params, f);
        JobDescription d;
        d.setRevision(0);
        bool success = r.read(d);
        assert(success);
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
        assert(d.getNumFormulaLiterals() > 0);
    }
}

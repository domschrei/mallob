

#include <cstdint>
#include <stdlib.h>
#include <unordered_set>
#include <iostream>
#include <vector>

#include "app/sat/data/formula_compressor.hpp"
#include "app/sat/parse/sat_reader.hpp"
#include "robin_map.h"
#include "robin_set.h"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/random.hpp"
#include "util/string_utils.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"

tsl::robin_map<int, std::vector<int>> getNormalizedFormula(const int* data, size_t size) {
    tsl::robin_map<int, std::vector<int>> litsPerClauseLength;
    std::vector<int> currentClause;
    for (size_t i = 0; i < size; i++) {
        int lit = data[i];
        if (lit == 0) {
            int clauseLength = currentClause.size();
            for (int j : currentClause)
                litsPerClauseLength[clauseLength].push_back(j);
            currentClause.clear();
        } else {
            currentClause.push_back(lit);
        }
    }
    return litsPerClauseLength;
}

void testFormulaCompression(Parameters& p, const std::string& f) {

    LOG(V2_INFO, "Reading test CNF %s ...\n", f.c_str());
    float time = Timer::elapsedSeconds();
    SatReader r(p, f);
    JobDescription d;
    d.beginInitialization(0);
    bool success = r.read(d);
    assert(success);
    time = Timer::elapsedSeconds() - time;
    LOG(V2_INFO, " - done, took %.3fs\n", time);

    LOG(V2_INFO, "Loaded formula of size %i : %s\n", d.getFormulaPayloadSize(0),
        StringUtils::getSummary(d.getFormulaPayload(0), d.getFormulaPayloadSize(0), 100).c_str());

    time = Timer::elapsedSeconds();
    FormulaCompressor comp;
    auto out = comp.compress(d.getFormulaPayload(0), d.getFormulaPayloadSize(0), nullptr, 0);
    auto compressed = *out.vec;
    float timeCompress = Timer::elapsedSeconds() - time;

    LOG(V2_INFO, "Compressed formula of size %i down to size %i : %s\n", d.getFormulaPayloadSize(0), compressed.size(),
        StringUtils::getSummary((unsigned char*) compressed.data(), sizeof(int)*compressed.size(), 100).c_str());

    time = Timer::elapsedSeconds();
    auto decompressed = comp.decompressViaView((unsigned char*) compressed.data(), sizeof(int)*compressed.size());
    float timeDecompress = Timer::elapsedSeconds() - time;

    LOG(V2_INFO, "Decompressed formula of size %i up to size %i : %s\n", compressed.size(), decompressed.size(),
        StringUtils::getSummary(decompressed, 100).c_str());

    LOG(V2_INFO, "Checking equivalence ...\n");
    auto normTrue = getNormalizedFormula(d.getFormulaPayload(0), d.getFormulaPayloadSize(0));
    auto normComp = getNormalizedFormula(decompressed.data(), decompressed.size());
    assert(normTrue.size() == normComp.size());
    for (auto& [len, litsTrue] : normTrue) {
        const auto& litsComp = normComp.at(len);
        assert(litsComp.size() == litsTrue.size() || log_return_false("[ERROR] literal sizes at clause length %i do not match!\n", len));
        assert(tsl::robin_set<int>(litsComp.begin(), litsComp.end()) == tsl::robin_set<int>(litsTrue.begin(), litsTrue.end())
            || log_return_false("[ERROR] literals at clause length %i do not match!\n%s\n%s\n",
                len, StringUtils::getSummary(litsTrue).c_str(), StringUtils::getSummary(litsComp).c_str()));
    }
    LOG(V2_INFO, "Equivalence checked successfully.\n");

    LOG(V2_INFO, "STATS %s %.4f %.4f %lu %lu %.4f\n", f.c_str(), timeCompress, timeDecompress, decompressed.size(), compressed.size(), decompressed.size()/(double)compressed.size());
}

int main(int argc, char *argv[]) {
    Timer::init();
    Random::init(rand(), rand());
    Logger::init(0, V5_DEBG);
    Process::init(0);
    ProcessWideThreadPool::init(4);

    Parameters params;
    params.init(argc, argv);

    testFormulaCompression(params, params.monoFilename());
}

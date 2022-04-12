
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>

#include "util/random.hpp"
#include "app/kmeans/kmeans_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

int main() {

    Timer::init();
    Logger::init(0, V5_DEBG, false, false, false, nullptr);

    auto files = {"benign_traffic.csv"};
    
    
    for (const auto& file : files) {
        auto f = std::string("instances/") + file;
        LOG(V2_INFO, "Reading test CNF %s ...\n", f.c_str());
        float time = Timer::elapsedSeconds();
        JobDescription desc;
        bool success = KMeansReader::read(f, desc);
        assert(success);
        const int* payload = desc.getFormulaPayload(0);
        std::cout << "K: " << payload[0] << std::endl;
        std::cout << "dim: " << payload[1] << std::endl;
        std::cout << "n: " << payload[2] << std::endl;
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
        assert(desc.getNumFormulaLiterals() > 0);
    }
}
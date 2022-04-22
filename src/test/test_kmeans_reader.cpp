
#include <iostream>
#include <string>
#include <vector>

#include "app/kmeans/kmeans_reader.hpp"
#include "app/kmeans/kmeans_job.hpp"
#include "app/kmeans/kmeans_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/sys/timer.hpp"

int main() {
    Timer::init();
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    auto files = {"mnist784.csv"};  //"benign_traffic.csv",

    for (const auto& file : files) {
        auto f = std::string("instances/") + file;
        LOG(V2_INFO, "Reading test KMeans File %s ...\n", f.c_str());
        float time = Timer::elapsedSeconds();
        JobDescription desc;
        bool success = KMeansReader::read(f, desc);
        assert(success);
        KMeansJob job(Parameters(), 1, 0, 0, desc.getFormulaPayload(0));

        job.loadInstance();

        LOG(V2_INFO, "K: %d \n", job.getNumClusters());
        LOG(V2_INFO, "Dimension %d \n", job.getDimension());
        LOG(V2_INFO, "Count of points %d \n", job.getPointsCount());

        std::stringstream lastPoint;

        for (auto e : job.getKMeansData()[job.getPointsCount() - 1]) {  // iterate over last point
            lastPoint << e << " ";
        }
        LOG(V2_INFO, "Last Point is: \n %s \n", lastPoint.str().c_str());
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
        assert(desc.getNumFormulaLiterals() > 0);
    }
}
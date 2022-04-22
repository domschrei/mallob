
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "app/kmeans/kmeans_job.hpp"
#include "app/kmeans/kmeans_reader.hpp"
#include "app/kmeans/kmeans_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/sys/timer.hpp"
typedef std::vector<float> Point;

int main() {
    Timer::init();
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    auto files = {
        "mnist784.csv",
        //"benign_traffic.csv",
        "covtype.csv",
        //"2d-10c.arff",
        //"birch-rg1.arff",
        //"birch-rg2.arff",
        //"birch-rg3.arff"
    };

    for (const auto& file : files) {
        auto f = std::string("../kMeansData/") + file;
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

        job.setRandomStartCenters();

        LOG(V2_INFO, "Start clusterCenters: \n%s\n", job.dataToString(job.getClusterCenters()).c_str());
        for (int i = 0; i < 5; ++i) {
            job.calcNearestCenter(
                [&](Point p1, Point p2) { return KMeansUtils::eukild(p1, p2); });

            std::stringstream countMembersString;
            job.countMembers();
            LOG(V2_INFO, "cluster membership counts: \n%s\n", job.dataToString(job.getClusterMembership()).c_str());

            job.calcCurrentClusterCenters();

            //LOG(V2_INFO, "New clusterCenters: \n%s\n", job.dataToString(job.getClusterCenters()).c_str());
        }
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
    }
}

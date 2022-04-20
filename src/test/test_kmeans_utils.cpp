
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "app/kmeans/kmeans_reader.hpp"
#include "app/kmeans/kmeans_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/random.hpp"
#include "util/sys/timer.hpp"

int main() {
    Timer::init();
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    auto files = {"mnist784.csv",
                  "benign_traffic.csv",
                  "covtype.csv",
                  "2d-10c.arff",
                  "birch-rg1.arff",
                  "birch-rg2.arff",
                  "birch-rg3.arff"};

    for (const auto& file : files) {
        auto f = std::string("../kMeansData/") + file;
        LOG(V2_INFO, "Reading test KMeans File %s ...\n", f.c_str());
        float time = Timer::elapsedSeconds();
        JobDescription desc;
        bool success = KMeansReader::read(f, desc);
        assert(success);
        const int* payload = desc.getFormulaPayload(0);

        KMeansUtils::KMeansInstance instance = KMeansUtils::loadPoints(desc);

        LOG(V2_INFO, "K: %d \n", instance.numClusters);
        LOG(V2_INFO, "Dimension %d \n", instance.dimension);
        LOG(V2_INFO, "Count of points %d \n", instance.pointsCount);

        KMeansUtils::ClusterCenters clusterCenters;
        clusterCenters.resize(instance.numClusters);
        for (int i = 0; i < instance.numClusters; ++i) {
            clusterCenters[i] = instance.data[static_cast<int>((static_cast<float>(i) / static_cast<float>(instance.numClusters)) * (instance.pointsCount - 1))];
        }

        LOG(V2_INFO, "Start clusterCenters: \n%s\n", KMeansUtils::pointsToString(clusterCenters).c_str());
        for (int i = 0; i < 5; ++i) {
            KMeansUtils::ClusterMembership clusterMembership;
            clusterMembership = KMeansUtils::calcNearestCenter(instance.data,
                                                               clusterCenters,
                                                               instance.pointsCount,
                                                               instance.numClusters,
                                                               [&](KMeansUtils::Point p1, KMeansUtils::Point p2) {return KMeansUtils::eukild(p1, p2);} );
            std::vector<int> countMembers(instance.numClusters, 0);
            for (int clusterID : clusterMembership) {
                countMembers[clusterID] += 1;
            }
            std::stringstream countMembersString;
            std::copy(countMembers.begin(), countMembers.end(), std::ostream_iterator<int>(countMembersString, " "));
            LOG(V2_INFO, "cluster membership counts: \n%s\n", countMembersString.str().c_str());
            clusterCenters = KMeansUtils::calcCurrentClusterCenters(instance.data,
                                                                    clusterMembership,
                                                                    instance.pointsCount,
                                                                    instance.numClusters,
                                                                    instance.dimension);
            // LOG(V2_INFO, "new clusterCenters: \n%s\n", KMeansUtils::pointsToString(clusterCenters).c_str());
        }
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
    }
}

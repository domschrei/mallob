
#include <iostream>
#include <string>
#include <vector>
#include <iterator>

#include "app/kmeans/kmeans_reader.hpp"
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
        const int* payload = desc.getFormulaPayload(0);

        KMeansUtils::KMeansInstance instance = KMeansUtils::loadPoints(desc);

        LOG(V2_INFO, "K: %d \n", instance.numClusters);
        LOG(V2_INFO, "Dimension %d \n", instance.dimension);
        LOG(V2_INFO, "Count of points %d \n", instance.pointsCount);

        KMeansUtils::ClusterCenters clusterCenters;
        clusterCenters.resize(instance.numClusters);
        for (int i = 0; i < instance.numClusters; ++i) {
            for (int j = 0; j < instance.dimension; ++j) {
                clusterCenters[i].push_back(i * instance.dimension + j);
            }
        }
        clusterCenters[0] = instance.data[3];
        clusterCenters[1] = instance.data[30];
        clusterCenters[2] = instance.data[60];
        clusterCenters[3] = instance.data[90];
        LOG(V2_INFO, "Start clusters: \n%s\n", KMeansUtils::pointsToString(clusterCenters).c_str());

        KMeansUtils::ClusterMembership clusterMembership;
        clusterMembership = KMeansUtils::calcNearestCenter(instance.data,
                                                           clusterCenters, 
                                                           instance.pointsCount, 
                                                           instance.numClusters,
                                                           KMeansUtils::eukild);
        std::vector<int> countMembers(instance.numClusters, 0);
        for (int clusterID : clusterMembership) {
            LOG(V2_INFO, "clusterMembership: %d\n", clusterID);
            countMembers[clusterID] += 1;
        }
        std::stringstream countMembersString;
        std::copy(countMembers.begin(), countMembers.end(), std::ostream_iterator<int>(countMembersString, " "));
        LOG(V2_INFO, "clusterMemberships: \n%s\n", countMembersString.str().c_str());
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
        assert(desc.getNumFormulaLiterals() > 0);
    }
}


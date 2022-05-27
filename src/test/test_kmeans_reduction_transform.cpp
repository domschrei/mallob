
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

bool equalsPointVectors(std::vector<Point>* fst, std::vector<Point>* snd) {
    const int count = fst->size();
    const int dim = (*fst)[0].size();
    if (snd->size() != count || (*snd)[0].size() != dim) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < dim; j++) {
            if ((*fst)[i][j] != (*snd)[i][j]) {
                return false;
            }
        }
    }
    return true;
}

int main() {
    Timer::init();
    Logger::init(0, V5_DEBG, false, false, false, nullptr);
    auto files = {
        "mnist784.csv",
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
        KMeansJob job(Parameters(), 1, 0, 0, desc.getFormulaPayload(0));

        job.loadInstance();

        LOG(V2_INFO, "K: %d \n", job.getNumClusters());
        LOG(V2_INFO, "Dimension %d \n", job.getDimension());
        LOG(V2_INFO, "Count of points %d \n", job.getPointsCount());

        job.setRandomStartCenters();

        LOG(V2_INFO, "Start clusterCenters: \n%s\n", job.dataToString(job.getClusterCenters()).c_str());
        while (1 / 1000 < job.calculateDifference(
                              [&](Point p1, Point p2) { return KMeansUtils::eukild(p1, p2); })) {
            job.calcNearestCenter(
                [&](Point p1, Point p2) { return KMeansUtils::eukild(p1, p2); }, 0);

            std::stringstream countMembersString;
            job.countMembers();
            LOG(V2_INFO, "cluster membership counts: \n%s\n", job.dataToString(job.getSumMembers()).c_str());

            job.calcCurrentClusterCenters();
            auto origCenters =job.getClusterCenters();
            std::vector<int> example1;
            example1.assign(job.getNumClusters(), 1);
            std::vector<int> example3;
            example3.assign(job.getNumClusters(), 3);
            auto reduceData = job.clusterCentersToReduce(job.getSumMembers(),job.getClusterCenters());
            auto reduceDataEX = job.clusterCentersToReduce(example3,job.getClusterCenters());
            LOG(V2_INFO, "rS: %d\n", reduceData.size());
            std::list<std::vector<int>> dblList;
            dblList.push_back(reduceData);
            dblList.push_back(reduceDataEX);
            auto dbl = job.aggregate(dblList);
            auto pair = job.reduceToclusterCenters(dbl);
            if (!equalsPointVectors(&pair.first, &origCenters)) {
                LOG(V2_INFO, "WRONG clusterCenters: \n%s\n", job.dataToString(pair.first).c_str());
                LOG(V2_INFO, "WRONG clusterCenters: \n%s\n", job.dataToString(origCenters).c_str());
            } else {
                LOG(V2_INFO, "RIGHT clusterCenters: \n%s\n", job.dataToString(pair.first).c_str());
            }
            // LOG(V2_INFO, "New clusterCenters: \n%s\n", job.dataToString(job.getClusterCenters()).c_str());
        }
        time = Timer::elapsedSeconds() - time;
        LOG(V2_INFO, " - done, took %.3fs\n", time);
    }
}
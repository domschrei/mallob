#include "kmeans_job.hpp"

#include <iostream>
#include <thread>

#include "app/job.hpp"
#include "comm/mympi.hpp"
#include "kmeans_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"
KMeansJob::KMeansJob(const Parameters& params, int commSize, int worldRank, int jobId, const int* newPayload)
    : Job(params, commSize, worldRank, jobId, JobDescription::Application::KMEANS) { setPayload(newPayload); }

void KMeansJob::appl_start() {
    calculating = ProcessWideThreadPool::get().addTask([&]() {
        payload = getDescription().getFormulaPayload(0);
        loadInstance();
        setRandomStartCenters();
        while (1 / 1000 < calculateDifference(
                              [&](Point p1, Point p2) { return KMeansUtils::eukild(p1, p2); })) {
            calcNearestCenter(
                [&](Point p1, Point p2) { return KMeansUtils::eukild(p1, p2); });

            calcCurrentClusterCenters();
        }
        internal_result.result = RESULT_SAT;
        

        internal_result.id = getId();
        internal_result.revision = getRevision();
        std::vector<int> example(5, 42);
        internal_result.setSolutionToSerialize(sumMembers.data(), sumMembers.size());
        finished = true;
    });
}
void KMeansJob::appl_suspend() {}
void KMeansJob::appl_resume() {}
JobResult&& KMeansJob::appl_getResult() {
    return std::move(internal_result);
}
void KMeansJob::appl_terminate() {}
void KMeansJob::appl_communicate() {}
void KMeansJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {}
void KMeansJob::appl_dumpStats() {}
void KMeansJob::appl_memoryPanic() {}
void KMeansJob::loadInstance() {
    numClusters = payload[0];
    dimension = payload[1];
    pointsCount = payload[2];
    kMeansData.reserve(pointsCount);
    payload += 3;  // pointer start at first datapoint instead of metadata
    for (int point = 0; point < pointsCount; ++point) {
        Point p;
        p.reserve(dimension);
        for (int entry = 0; entry < dimension; ++entry) {
            p.push_back(*((float*)(payload + entry)));
        }
        kMeansData.push_back(p);
        payload = payload + dimension;
    }
}

void KMeansJob::setRandomStartCenters() {
    clusterCenters.clear();
    clusterCenters.resize(numClusters);
    for (int i = 0; i < numClusters; ++i) {
        clusterCenters[i] = kMeansData[static_cast<int>((static_cast<float>(i) / static_cast<float>(numClusters)) * (pointsCount - 1))];
    }
}

void KMeansJob::calcNearestCenter(std::function<float(Point, Point)> metric) {
    struct centerDistance {
        int cluster;
        float distance;
    } currentNearestCenter;
    clusterMembership.assign(pointsCount, 0);
    float distanceToCluster;
    for (int pointID = 0; pointID < pointsCount; ++pointID) {
        currentNearestCenter.cluster = -1;
        currentNearestCenter.distance = std::numeric_limits<float>::infinity();
        for (int clusterID = 0; clusterID < numClusters; ++clusterID) {
            distanceToCluster = metric(kMeansData[pointID], clusterCenters[clusterID]);
            if (distanceToCluster < currentNearestCenter.distance) {
                currentNearestCenter.cluster = clusterID;
                currentNearestCenter.distance = distanceToCluster;
            }
        }
        clusterMembership[pointID] = currentNearestCenter.cluster;
    }
}

void KMeansJob::calcCurrentClusterCenters() {
    oldClusterCenters = clusterCenters;
    typedef std::vector<float> Dimension;                             // transposed data to reduce dimension by dimension
    typedef std::vector<std::vector<Dimension>> ClusteredDataPoints;  // ClusteredDataPoints[i] contains the points belonging to cluster i
    ClusteredDataPoints clusterdPoints;

    clusterdPoints.resize(numClusters);
    for (int cluster = 0; cluster < numClusters; ++cluster) {
        clusterdPoints[cluster].resize(dimension);
    }
    for (int pointID = 0; pointID < pointsCount; ++pointID) {
        for (int d = 0; d < dimension; ++d) {
            clusterdPoints[clusterMembership[pointID]][d].push_back(kMeansData[pointID][d]);
        }
    }
    for (int cluster = 0; cluster < numClusters; ++cluster) {
        for (int d = 0; d < dimension; ++d) {
            for (int elem = 0; elem < clusterdPoints[cluster][0].size(); ++elem) {
                clusterdPoints[cluster][d][elem] /= static_cast<float>(clusterdPoints[cluster][0].size());
            }
        }
    }
    clusterCenters.clear();
    clusterCenters.resize(numClusters);
    for (int cluster = 0; cluster < numClusters; ++cluster) {
        for (int d = 0; d < dimension; ++d) {
            clusterCenters[cluster].push_back(std::reduce(clusterdPoints[cluster][d].begin(),
                                                          clusterdPoints[cluster][d].end()));
        }
    }
    ++iterationsDone;
}

std::string KMeansJob::dataToString(std::vector<Point> data) {
    std::stringstream result;

    for (auto point : data) {
        for (auto entry : point) {
            result << entry << " ";
        }
        result << "\n";
    }
    return result.str();
}
std::string KMeansJob::dataToString(std::vector<int> data) {
    std::stringstream result;

    for (auto entry : sumMembers) {
        result << entry << " ";
    }
    result << "\n";
    return result.str();
}

void KMeansJob::countMembers() {
    std::stringstream result;
    sumMembers.assign(numClusters, 0);
    for (int clusterID : clusterMembership) {
        sumMembers[clusterID] += 1;
    }
}

float KMeansJob::calculateDifference(std::function<float(Point, Point)> metric) {
    if (iterationsDone == 0) {
        return std::numeric_limits<float>::infinity();
    }
    float sumOldvec = 0.0;
    float sumDifference = 0.0;
    Point v0(dimension, 0);
    for (int k = 0; k < numClusters; ++k) {
        sumOldvec += metric(v0, clusterCenters[k]);

        sumDifference += metric(clusterCenters[k], oldClusterCenters[k]);
    }
    return sumDifference / sumOldvec;
}
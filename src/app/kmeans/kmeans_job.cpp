#include "kmeans_job.hpp"

#include <iostream>
#include <thread>

#include "app/job.hpp"
#include "app/job_tree.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "comm/mympi.hpp"
#include "kmeans_utils.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/sys/proc.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"
KMeansJob::KMeansJob(const Parameters& params, int commSize, int worldRank, int jobId, const int* newPayload)
    : Job(params, commSize, worldRank, jobId, JobDescription::Application::KMEANS) {
    setPayload(newPayload);
    auto folder =
        [&](std::list<std::vector<int>>& elems) {
            return std::vector<int>(0, 0);
        };
    JobTreeAllReduction red(getJobTree(),
                            JobMessage(getId(),
                                       getRevision(),
                                       0,
                                       MSG_ALLREDUCE_CLAUSES),
                            std::vector<int>(0, 0),
                            folder);
    reducer = &red;
}

void KMeansJob::appl_start() {
    LOG(V0_CRIT, "commSize: %i worldRank: %i \n", getGlobalNumWorkers(), getMyMpiRank());
    calculating = ProcessWideThreadPool::get().addTask([&]() {
        auto metric = [&](Point p1, Point p2) { return KMeansUtils::eukild(p1, p2); };
        payload = getDescription().getFormulaPayload(0);
        loadInstance();
        allReduceElementSize =  dimension * (countClusters + 1);
        setRandomStartCenters();

        while (1 / 1000 < calculateDifference(metric)) {
            calcNearestCenter(metric);
            calcCurrentClusterCenters();
        }

        internal_result.result = RESULT_SAT;
        internal_result.id = getId();
        internal_result.revision = getRevision();
        std::vector<int> transformSolution;
        /*
        transformSolution.reserve(sumMembers.size() + 1);
        transformSolution.push_back(-42);
        for (auto element : sumMembers) {
            transformSolution.push_back(element);
        }
        */
        internal_result.encodedType = JobResult::EncodedType::FLOAT;
        auto solution = clusterCentersToSolution();
        internal_result.setSolutionToSerialize((int*)(solution.data()), solution.size());
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
    countClusters = payload[0];
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
    clusterCenters.resize(countClusters);
    for (int i = 0; i < countClusters; ++i) {
        clusterCenters[i] = kMeansData[static_cast<int>((static_cast<float>(i) / static_cast<float>(countClusters)) * (pointsCount - 1))];
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
        for (int clusterID = 0; clusterID < countClusters; ++clusterID) {
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

    countMembers();

    for (int cluster = 0; cluster < countClusters; ++cluster) {
        clusterCenters[cluster].assign(dimension, 0);
    }
    for (int pointID = 0; pointID < pointsCount; ++pointID) {
        for (int d = 0; d < dimension; ++d) {
            clusterCenters[clusterMembership[pointID]][d] +=
                kMeansData[pointID][d] / static_cast<float>(sumMembers[clusterMembership[pointID]]);
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
    sumMembers.assign(countClusters, 0);
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
    for (int k = 0; k < countClusters; ++k) {
        sumOldvec += metric(v0, clusterCenters[k]);

        sumDifference += metric(clusterCenters[k], oldClusterCenters[k]);
    }
    return sumDifference / sumOldvec;
}

std::vector<float> KMeansJob::clusterCentersToSolution() {
    std::vector<float> result;
    result.push_back((float)countClusters);
    result.push_back((float)dimension);

    for (auto point : clusterCenters) {
        for (auto entry : point) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<int> KMeansJob::clusterCentersToReduce() {
    std::vector<int> result;
    result.reserve(allReduceElementSize);
    for (auto entry : localSumMembers) {
        result.push_back(entry);
    }
    for (auto point : localClusterCenters) {
        auto centerData = point.data();
        for (int entry = 0; entry < dimension; ++entry) {
            result.push_back(*((int*)(centerData + entry)));
        }
    }
    return result;
}

std::vector<std::vector<float>> KMeansJob::reduceToclusterCenters() {
}
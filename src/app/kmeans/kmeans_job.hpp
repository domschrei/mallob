
#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "app/job.hpp"
#include "app/sat/job/sat_constants.h"
#include "comm/job_tree_all_reduction.hpp"
#include "kmeans_utils.hpp"
#include "util/params.hpp"

class KMeansJob : public Job {
   private:
    typedef std::vector<float> Point;
    std::vector<Point> clusterCenters;       // The centers of cluster 0..n
    std::vector<Point> localClusterCenters;  // The centers of cluster 0..n
    std::vector<Point> oldClusterCenters;
    std::vector<int> clusterMembership;  // A point KMeansData[i] belongs to cluster ClusterMembership[i]
    
    std::vector<int> localSumMembers;
    int countClusters;
    int dimension;
    int pointsCount;
    int allReduceElementSize;
    int iterationsDone = 0;
    const int epoch = 0;  // should count the message epoch, but does not do it currently
    int maxDemand = -1;
    bool maxDemandCalculated = false;
    uint8_t* data;
    const float* pointsStart;
    std::future<void> calculatingTask;
    std::future<void> initMsgTask;
    bool finishedJob = false;
    bool iAmRoot = false;
    bool loaded = false;
    bool initSend = false;
    bool calculatingFinished = false;
    bool hasReducer = false;
    bool leftDone = false;
    bool rightDone = false;
    bool skipCurrentIter = false;
    bool terminate = false;
    std::vector<int> work;
    std::vector<int> workDone;
    int myRank;
    int myIndex = -2;
    int countCurrentWorkers;
    JobMessage baseMsg;
    JobResult internal_result;
    std::unique_ptr<JobTreeAllReduction> reducer;
    const std::function<float(const float* p1, const float* p2, const size_t dim)> metric =
        [&](const float* p1, const float* p2, const size_t dim) {
            return KMeansUtils::eukild(p1, p2, dim);
        };
    const std::function<std::vector<int>(std::list<std::vector<int>>&)> folder =
        [&](std::list<std::vector<int>>& elems) {
            return aggregate(elems);
        };
    const std::function<std::vector<int>(const std::vector<int>&)> rootTransform =
        [&](const std::vector<int>& payload) {
            LOG(V3_VERB, "                           myIndex: %i start Roottransform\n", myIndex);
            auto data = reduceToclusterCenters(payload);
            const int* sumMembers;

            clusterCenters = data.first;
            sumMembers = data.second.data();
            int sum = 0;
            for (int i = 0; i < countClusters; ++i) {
                sum += sumMembers[i];
            }
            if (sum == pointsCount) {
                LOG(V3_VERB, "                           AllCollected: Good\n");
            } else {
                LOG(V3_VERB, "                           AllCollected: Error\n");
            }
            auto transformed = clusterCentersToBroadcast(clusterCenters);
            transformed.push_back(this->getVolume());  // will be countCurrentWorkers
            LOG(V3_VERB, "                           COMMSIZE: %i myIndex: %i \n",
                this->getVolume(), myIndex);
            LOG(V3_VERB, "                           Children: %i\n",
                this->getJobTree().getNumChildren());

            if (!centersChanged(0.01f)||iterationsDone >= 150) {
                LOG(V0_CRIT, "                           Got Result after iter %i\n", iterationsDone);
                internal_result.result = RESULT_SAT;
                internal_result.id = getId();
                internal_result.revision = getRevision();
                std::vector<int> transformSolution;

                internal_result.encodedType = JobResult::EncodedType::FLOAT;
                auto solution = clusterCentersToSolution();
                internal_result.setSolutionToSerialize((int*)(solution.data()), solution.size());
                finishedJob = true;
                LOG(V2_INFO, "Solution clusterCenters: \n%s\n", dataToString(clusterCenters).c_str());
                return std::move(std::vector<int>(allReduceElementSize, 0));

            } else {
                if (iAmRoot && iterationsDone == 1) {
                    LOG(V0_CRIT, "                           first iteration finished\n");
                }
                LOG(V2_INFO, "                           Another iter %i    k:%i    w:%i\n", iterationsDone, countClusters, this->getVolume());
                //LOG(V2_INFO, "                           Another iter %i    k:%i    w:%i   dem:%i\n", iterationsDone, countClusters, this->getVolume(), this->getDemand());
                return transformed;
            }
        };

   public:
    std::vector<Point> getClusterCenters() { return clusterCenters; };      // The centers of cluster 0..n
    std::vector<int> getClusterMembership() { return clusterMembership; };  // A point KMeansData[i] belongs to cluster ClusterMembership[i]
    //const int* getSumMembers() { return sumMembers; };
    int getNumClusters() { return countClusters; };
    int getDimension() { return dimension; };
    int getPointsCount() { return pointsCount; };
    int getIterationsDone() { return iterationsDone; };

    KMeansJob(const Parameters& params, int commSize, int worldRank, int jobId);
    void appl_start() override;
    void appl_suspend() override;
    void appl_resume() override;
    void appl_terminate() override;
    ~KMeansJob();
    int appl_solved() override { return finishedJob ? RESULT_SAT : -1; }  // atomic bool
    // int getDemand() const { return 1; }
    JobResult&& appl_getResult() override;
    void appl_communicate() override;
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override;
    void appl_dumpStats() override;
    bool appl_isDestructible() override { return true; }
    void appl_memoryPanic() override;
    int getDemand() const override {
        return Job::getDemand();
        if (!loaded) {
            return Job::getDemand();
        } else {
            return maxDemand;
        }
    }

    void setMaxDemand() {
        double problemSize = countClusters * dimension * pointsCount;
                maxDemand = 0.00005410212640549420 * pow(problemSize, 0.68207843808596479995);  // evaluated with tests
                maxDemandCalculated = true;
                if (maxDemand <= 0) {
                    maxDemand = 1;
                } else {
                    uint32_t positiveDemand = maxDemand;
                    int lowerBound = 1;
                    while (positiveDemand >>= 1) {
                        lowerBound <<= 1;
                    }
                    int middle = lowerBound * 1.5;
                    if (maxDemand > middle) {
                        maxDemand = lowerBound * 2 - 1;
                    } else {
                        maxDemand = lowerBound - 1;
                    }
                }
                maxDemand = std::min(maxDemand, Job::getGlobalNumWorkers());
    }

    void loadInstance();
    void doInitWork();
    void sendRootNotification();
    void setRandomStartCenters();
    void calcNearestCenter(std::function<float(const float* p1, const float* p2, const size_t dim)> metric, int intervalId);
    void calcCurrentClusterCenters();
    std::string dataToString(std::vector<Point> data);
    std::string dataToString(std::vector<int> data);
    void countMembers();
    float calculateDifference(std::function<float(const float* p1, const float* p2, const size_t dim)> metric);
    bool centersChanged();
    bool centersChanged(float factor);
    std::vector<float> clusterCentersToSolution();
    std::vector<int> clusterCentersToBroadcast(const std::vector<Point>&);
    std::vector<Point> broadcastToClusterCenters(const std::vector<int>&);
    void setClusterCenters(const std::vector<int>&);
    std::vector<int> clusterCentersToReduce(const std::vector<int>&, const std::vector<Point>&);
    std::pair<std::vector<std::vector<float>>, std::vector<int>> reduceToclusterCenters(const std::vector<int>&);
    std::vector<int> aggregate(const std::list<std::vector<int>>&);
    void advanceCollective(JobMessage& msg, JobTree& jobTree);
    void initReducer(JobMessage& msg);
    int getIndex(int rank);
    const float* getKMeansData(int point) {
        return (pointsStart + dimension * point);
    }
    float getEntry(int entry);
};

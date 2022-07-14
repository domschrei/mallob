
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
    std::vector<int> sumMembers;
    std::vector<int> localSumMembers;
    int countClusters;
    int dimension;
    int pointsCount;
    int allReduceElementSize;
    int iterationsDone = 0;
    int epoch = 0;
    int countReady = 0;
    std::vector<Point> kMeansData;
    uint8_t* data;
    const float* pointsStart;
    std::future<void> calculatingTask;
    std::future<void> initMsgTask;
    bool finishedJob = false;
    bool iAmRoot = false;
    bool loaded = false;
    bool initSend = false;
    bool receivedInitSend = false;
    bool calculatingFinished = false;
    bool allCollected = false;
    bool hasReducer = false;
    bool leftDone = false;
    bool rightDone = false;
    bool skipCurrentIter = false;
    bool terminate = false;
    std::vector<int> work;
    std::vector<int> workDone;
    std::pair<bool, bool> childsFinished = {false, false};
    int myRank;
    int myIndex = -2;
    int countCurrentWorkers;
    JobMessage baseMsg;
    JobResult internal_result;
    std::unique_ptr<JobTreeAllReduction> reducer;
    std::function<float(const float*, KMeansJob::Point)> metric = [&](const float* p1, Point p2) { return KMeansUtils::eukild(p1, p2); };
    std::function<std::vector<int>(std::list<std::vector<int>>&)> folder =
        [&](std::list<std::vector<int>>& elems) {
            return aggregate(elems);
        };
    std::function<std::vector<int>(const std::vector<int>&)> rootTransform = [&](const std::vector<int>& payload) {
        LOG(V3_VERB, "                           myIndex: %i start Roottransform\n", myIndex);
        auto data = reduceToclusterCenters(payload);

        clusterCenters = data.first;
        sumMembers = data.second;
        int sum = 0;
        for (auto i : sumMembers) {
            sum += i;
        }
        if (sum == pointsCount) {
            allCollected = true;

            LOG(V3_VERB, "                           AllCollected: Good\n");
        } else {
            LOG(V3_VERB, "                           AllCollected: Error\n");
        }
        auto transformed = clusterCentersToBroadcast(clusterCenters);
        transformed.push_back(this->getVolume()); // will be countCurrentWorkers
        LOG(V3_VERB, "                           COMMSIZE: %i myIndex: %i \n",
            this->getVolume(), myIndex);
        LOG(V3_VERB, "                           Children: %i\n",
            this->getJobTree().getNumChildren());

        if (!centersChanged(metric, 0.001f)) {
            LOG(V0_CRIT, "                           Got Result after iter %i\n", iterationsDone);
            internal_result.result = RESULT_SAT;
            internal_result.id = getId();
            internal_result.revision = getRevision();
            std::vector<int> transformSolution;

            internal_result.encodedType = JobResult::EncodedType::FLOAT;
            auto solution = clusterCentersToSolution();
            internal_result.setSolutionToSerialize((int*)(solution.data()), solution.size());
            finishedJob = true;
            LOG(V3_VERB, "Solution clusterCenters: \n%s\n", dataToString(clusterCenters).c_str());
            return std::move(std::vector<int>(allReduceElementSize, 0));

        } else {
            if (iAmRoot && iterationsDone == 1) {
                LOG(V0_CRIT, "                           first iteration finished\n");
            }
            LOG(V3_VERB, "                           Another iter %i\n", iterationsDone);
            return transformed;
        }
    };

   public:
    std::vector<Point> getClusterCenters() { return clusterCenters; };      // The centers of cluster 0..n
    std::vector<int> getClusterMembership() { return clusterMembership; };  // A point KMeansData[i] belongs to cluster ClusterMembership[i]
    std::vector<int> getSumMembers() { return sumMembers; };
    int getNumClusters() { return countClusters; };
    int getDimension() { return dimension; };
    int getPointsCount() { return pointsCount; };
    int getIterationsDone() { return iterationsDone; };
    std::vector<Point> getKMeansData() { return kMeansData; };

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

    void loadInstance();
    void doInitWork();
    void sendRootNotification();
    void setRandomStartCenters();
    void calcNearestCenter(std::function<float(const float*, Point)> metric, int intervalId);
    void calcCurrentClusterCenters();
    std::string dataToString(std::vector<Point> data);
    std::string dataToString(std::vector<int> data);
    void countMembers();
    float calculateDifference(std::function<float(const float*, Point)> metric);
    bool centersChanged();
    bool centersChanged(std::function<float(const float*, Point)> metric, float factor);
    std::vector<float> clusterCentersToSolution();
    std::vector<int> clusterCentersToBroadcast(const std::vector<Point>&);
    std::vector<Point> broadcastToClusterCenters(const std::vector<int>&, bool withNumWorkers = false);
    std::vector<int> clusterCentersToReduce(const std::vector<int>&, const std::vector<Point>&);
    std::pair<std::vector<std::vector<float>>, std::vector<int>> reduceToclusterCenters(const std::vector<int>&);
    std::vector<int> aggregate(std::list<std::vector<int>>);
    void advanceCollective(JobMessage& msg, JobTree& jobTree);
    void initReducer(JobMessage& msg);
    int getIndex(int rank);
    const float* getKMeansData(int point) {
        return (pointsStart + dimension * point);
    }
    float getEntry(int entry);
};

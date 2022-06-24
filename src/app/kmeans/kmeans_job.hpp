
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
    const int* payload;
    std::future<void> calculatingTask;
    std::future<void> loadTask;
    std::future<void> initMsgTask;
    std::vector<int> myIntervalStarts;
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
    bool iDoLeft = false;
    bool iDoRight = false;
    std::vector<int> work;
    std::mutex mWork;
    std::pair<bool, bool> childsFinished = {false, false};
    int myRank;
    int myIndex;
    int countCurrentWorkers;
    int maxGrandchildIndex;
    float diff = 1;
    JobMessage baseMsg;
    JobResult internal_result;
    std::unique_ptr<JobTreeAllReduction> reducer;
    std::function<float(KMeansJob::Point, KMeansJob::Point)> metric = [&](Point p1, Point p2) { return KMeansUtils::eukild(p1, p2); };
    std::function<std::vector<int>(std::list<std::vector<int>>&)> folder =
        [&](std::list<std::vector<int>>& elems) {
            return aggregate(elems);
        };
    std::function<std::vector<int>(std::vector<int>)> rootTransform = [&](std::vector<int> payload) {
        LOG(V2_INFO, "                           myIndex: %i start Roottransform\n", myIndex);
        auto data = reduceToclusterCenters(payload);

        clusterCenters = data.first;
        sumMembers = data.second;
        int sum = 0;
        for (auto i : sumMembers) {
            sum += i;
        }
        if (sum == pointsCount) {
            allCollected = true;

            LOG(V2_INFO, "                           AllCollected: Good\n");
        } else {
            LOG(V2_INFO, "                           AllCollected: Error\n");
        }
        auto transformed = clusterCentersToBroadcast(clusterCenters);
        transformed.push_back(this->getVolume());
        LOG(V2_INFO, "                           COMMSIZE: %i myIndex: %i \n",
            this->getVolume(), myIndex);
        LOG(V2_INFO, "                           Children: %i\n",
            this->getJobTree().getNumChildren());
        diff = calculateDifference(
            [&](Point p1, Point p2) { return KMeansUtils::eukild(p1, p2); });
        if ((0.001f < diff)) {
            LOG(V2_INFO, "                           Another iter %i\n", iterationsDone);
            return transformed;

        } else {
            LOG(V2_INFO, "                           Got Result after iter %i\n", iterationsDone);
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
    const int* getPayload() { return payload; };
    void setPayload(const int* newPayload) { payload = newPayload; };

    KMeansJob(const Parameters& params, int commSize, int worldRank, int jobId, const int* newPayload);
    void appl_start() override;
    void appl_suspend() override;
    void appl_resume() override;
    void appl_terminate() override;
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
    void calcNearestCenter(std::function<float(Point, Point)> metric, int intervalId);
    void calcCurrentClusterCenters();
    std::string dataToString(std::vector<Point> data);
    std::string dataToString(std::vector<int> data);
    void countMembers();
    float calculateDifference(std::function<float(Point, Point)> metric);
    std::vector<float> clusterCentersToSolution();
    std::vector<int> clusterCentersToBroadcast(std::vector<Point>);
    std::vector<Point> broadcastToClusterCenters(std::vector<int>, bool withNumWorkers = false);
    std::vector<int> clusterCentersToReduce(std::vector<int>, std::vector<Point>);
    std::pair<std::vector<std::vector<float>>, std::vector<int>> reduceToclusterCenters(std::vector<int>);
    std::vector<int> aggregate(std::list<std::vector<int>>);
    void advanceCollective(JobMessage& msg, JobTree& jobTree);
    void initReducer(JobMessage& msg);
    int getIndex(int rank);
};

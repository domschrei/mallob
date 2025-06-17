
#pragma once

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "app/job.hpp"
#include "app/sat/job/sat_constants.h"
#include "comm/job_tree_basic_all_reduction.hpp"
#include "kmeans_utils.hpp"
#include "util/params.hpp"

class KMeansJob : public Job {
   private:
    typedef std::vector<float> Point;

    std::vector<Point> _cluster_centers;       // The centers of cluster 0..n
    std::vector<Point> _local_cluster_centers;  // The centers of cluster 0..n
    std::vector<Point> _old_cluster_centers;
    std::vector<int> _cluster_membership;  // A point KMeansData[i] belongs to cluster ClusterMembership[i]
    std::vector<int> _local_sum_members;

    int _num_clusters;
    int _dimension;
    int _num_points;
    int _all_red_elem_size;

    int _iterations_done = 0;

    int _max_demand = -1;
    bool _max_demand_calculated = false;
    uint8_t* _data;
    const float* _points_start;
    std::future<void> _calculating_task;
    std::future<void> _init_msg_task;
    bool _finished_job = false;
    bool _is_root = false;
    bool _loaded = false;
    bool _init_send = false;
    bool _calculating_finished = false;
    bool _has_reducer = false;
    bool _left_done = false;
    bool _right_done = false;
    bool _skip_current_iter = false;
    bool _terminate = false;
    std::vector<int> _work;
    std::vector<int> _work_done;
    int _my_rank;
    int _my_index = -2;
    int _num_curr_workers;
    JobMessage _base_msg;
    JobResult _internal_result;
    std::unique_ptr<JobTreeBasicAllReduction> _reducer;

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
            LOG(V5_DEBG, "KMDBG myIndex: %i start Roottransform\n", _my_index);
            auto data = reduceToclusterCenters(payload);
            const int* sumMembers;

            _cluster_centers = data.first;
            sumMembers = data.second.data();
            int sum = 0;
            for (int i = 0; i < _num_clusters; ++i) {
                sum += sumMembers[i];
            }
            
            auto transformed = clusterCentersToBroadcast(_cluster_centers);
            transformed.push_back(this->getVolume());  // will be countCurrentWorkers
            LOG(V5_DEBG, "KMDBG COMMSIZE: %i myIndex: %i \n",
                this->getVolume(), _my_index);
            LOG(V5_DEBG, "KMDBG Children: %i\n",
                this->getJobTree().getNumChildren());

            if (!centersChanged(0.001f)) {
                LOG(V2_INFO, "%s : finished after %i iterations\n", toStr(), _iterations_done);
                _internal_result.result = RESULT_SAT;
                _internal_result.id = getId();
                _internal_result.revision = getRevision();
                std::vector<int> transformSolution;

                _internal_result.encodedType = JobResult::EncodedType::FLOAT;
                auto solution = clusterCentersToSolution();
                _internal_result.setSolutionToSerialize((int*)(solution.data()), solution.size());
                _finished_job = true;
                LOG(V5_DEBG, "%s : solution cluster centers: \n%s\n", toStr(), dataToString(_cluster_centers).c_str());
                return std::move(std::vector<int>(_all_red_elem_size, 0));

            } else {
                if (_is_root && _iterations_done == 1) {
                    LOG(V5_DEBG, "KMDBG first iteration finished\n");
                }
                LOG(V3_VERB, "%s : Iteration %i - k:%i w:%i\n", toStr(), _iterations_done, _num_clusters, this->getVolume());
                //LOG(V2_INFO, "KMDBG Another iter %i    k:%i    w:%i   dem:%i\n", iterationsDone, countClusters, this->getVolume(), this->getDemand());
                return transformed;
            }
        };


   public:
    std::vector<Point> getClusterCenters() { return _cluster_centers; };      // The centers of cluster 0..n
    std::vector<int> getClusterMembership() { return _cluster_membership; };  // A point KMeansData[i] belongs to cluster ClusterMembership[i]
    //const int* getSumMembers() { return sumMembers; };
    int getNumClusters() { return _num_clusters; };
    int getDimension() { return _dimension; };
    int getPointsCount() { return _num_points; };
    int getIterationsDone() { return _iterations_done; };

    KMeansJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table);
    void appl_start() override;
    void appl_suspend() override;
    void appl_resume() override;
    void appl_terminate() override;
    ~KMeansJob();
    int appl_solved() override { return _finished_job ? RESULT_SAT : -1; }  // atomic bool
    // int getDemand() const { return 1; }
    JobResult&& appl_getResult() override;
    void appl_communicate() override;
    void appl_communicate(int source, int mpiTag, JobMessage& msg) override;
    void appl_dumpStats() override;
    bool appl_isDestructible() override { return true; }
    void appl_memoryPanic() override;
    int getDemand() const override {
        //return Job::getDemand();
        if (!_loaded) {
            return Job::getDemand();
        } else {
            return _max_demand;
        }
    }

    void setMaxDemand() {
        double problemSize = _num_clusters * _dimension * _num_points;
                _max_demand = 0.00005410212640549420 * pow(problemSize, 0.68207843808596479995);  // evaluated with tests
                _max_demand_calculated = true;
                if (_max_demand <= 0) {
                    _max_demand = 1;
                } else {
                    uint32_t positiveDemand = _max_demand;
                    int lowerBound = 1;
                    while (positiveDemand >>= 1) {
                        lowerBound <<= 1;
                    }
                    int middle = lowerBound * 1.5;
                    if (_max_demand > middle) {
                        _max_demand = lowerBound * 2 - 1;
                    } else {
                        _max_demand = lowerBound - 1;
                    }
                }
                _max_demand = std::min(_max_demand, Job::getGlobalNumWorkers());
    }

    void reset();
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
        return (_points_start + _dimension * point);
    }
    float getEntry(int entry);
};

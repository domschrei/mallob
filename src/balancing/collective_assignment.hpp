
#ifndef DOMPASCH_MALLOB_COLLECTIVE_ASSIGNMENT_HPP
#define DOMPASCH_MALLOB_COLLECTIVE_ASSIGNMENT_HPP

#include <set>
#include <functional>

#include "data/job_transfer.hpp"
#include "comm/mympi.hpp"

class JobDatabase; // forward declaration

class CollectiveAssignment {

private:
    JobDatabase* _job_db = nullptr;
    std::function<void(const JobRequest&, int)> _local_request_callback;
    
    struct Status {
        int numIdle;
    };
    robin_hood::unordered_map<int, Status> _child_statuses;
    std::set<JobRequest> _request_list;

    int _num_workers;
    std::vector<int> _neighbor_towards_rank;
    
    int _epoch = -1;
    bool _status_dirty = true;

public:
    CollectiveAssignment() {}
    CollectiveAssignment(JobDatabase& jobDb, int numWorkers, std::vector<int>&& neighborTowardsRank, 
    std::function<void(const JobRequest&, int)> localRequestCallback) : 
        _job_db(&jobDb), _local_request_callback(localRequestCallback), _num_workers(numWorkers), 
        _neighbor_towards_rank(std::move(neighborTowardsRank)) {}

    void handle(MessageHandle& handle);

    Status getAggregatedStatus();
    std::vector<uint8_t> serialize(const Status& status);
    std::vector<uint8_t> serialize(const std::vector<JobRequest>& requests);
    void deserialize(const std::vector<uint8_t>& packed, int source);

    void setStatusDirty();
    void addJobRequest(JobRequest& request);

    void advance(int epoch);

    void resolveRequests();

    int getCurrentRoot();
    int getCurrentParent();

    bool isIdle();

private:
    int getDestination();

};

#endif

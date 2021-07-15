
#ifndef DOMPASCH_MALLOB_COLLECTIVE_ASSIGNMENT_HPP
#define DOMPASCH_MALLOB_COLLECTIVE_ASSIGNMENT_HPP

#include <set>

#include "data/job_transfer.hpp"
#include "comm/mympi.hpp"

class JobDatabase; // forward declaration

class CollectiveAssignment {

private:
    JobDatabase* _job_db;
    
    struct Status {
        int numIdle;
        robin_hood::unordered_map<int, int> numCachedPerJob;
    };
    robin_hood::unordered_map<int, Status> _child_statuses;
    std::set<JobRequest> _request_list;

    int _num_workers;
    std::vector<int> _neighbor_towards_rank;
    
    int _epoch = -1;
    bool _status_dirty = true;

public:
    CollectiveAssignment() {}
    CollectiveAssignment(JobDatabase& jobDb, int numWorkers, std::vector<int>&& neighborTowardsRank) : 
        _job_db(&jobDb), _num_workers(numWorkers), _neighbor_towards_rank(std::move(neighborTowardsRank)) {}

    void handle(MessageHandle& handle);

    Status getAggregatedStatus();
    std::vector<uint8_t> serialize(const Status& status);
    std::vector<uint8_t> serializeRequests();
    void deserialize(const std::vector<uint8_t>& packed, int source);

    void setStatusDirty();
    void addJobRequest(JobRequest& request);

    void advance(int epoch);

    void resolveRequests();

    int getCurrentRoot();
    int getCurrentParent();
};

#endif

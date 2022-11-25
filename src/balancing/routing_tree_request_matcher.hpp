
#pragma once

#include <set>
#include <functional>

#include "data/job_transfer.hpp"
#include "comm/mympi.hpp"
#include "request_matcher.hpp"
#include "comm/randomized_routing_tree.hpp"

class RoutingTreeRequestMatcher : public RequestMatcher {

private:
    struct Status {
        int numIdle;
    };
    robin_hood::unordered_map<int, Status> _child_statuses;
    std::set<JobRequest> _request_list;

    RandomizedRoutingTree& _tree;
    
public:
    RoutingTreeRequestMatcher(JobRegistry& jobRegistry, MPI_Comm workersComm, 
            RandomizedRoutingTree& tree, 
            std::function<void(const JobRequest&, int)> localRequestCallback) : 
        RequestMatcher(jobRegistry, workersComm, localRequestCallback),
        _tree(tree) {}
    virtual ~RoutingTreeRequestMatcher() {}

    virtual void handle(MessageHandle& handle) override;
    virtual void advance(int epoch) override;
    virtual void addJobRequest(JobRequest& request) override;

private:
    Status getAggregatedStatus();

    std::vector<uint8_t> serialize(const Status& status);
    std::vector<uint8_t> serialize(const std::vector<JobRequest>& requests);
    void deserialize(const std::vector<uint8_t>& packed, int source);

    void resolveRequests();

    int getDestination();
};

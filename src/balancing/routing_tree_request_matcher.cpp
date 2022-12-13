
#include "routing_tree_request_matcher.hpp"

#include "util/assert.hpp"

#include "util/logger.hpp"
#include "core/scheduling_manager.hpp"
#include "util/random.hpp"

const uint8_t COLL_ASSIGN_STATUS = 1;
const uint8_t COLL_ASSIGN_REQUESTS = 2;

void RoutingTreeRequestMatcher::handle(MessageHandle& handle) {
    deserialize(handle.getRecvData(), handle.source);
}

std::vector<uint8_t> RoutingTreeRequestMatcher::serialize(const Status& status) {
    std::vector<uint8_t> packed(1 + 2*sizeof(int));
    int i = 0, n;
    n = 1; memcpy(packed.data() + i, &COLL_ASSIGN_STATUS, n); i += n;
    n = sizeof(int);
    memcpy(packed.data() + i, &_epoch, n); i += n;
    memcpy(packed.data() + i, &status.numIdle, n); i += n;
    return packed;
}

std::vector<uint8_t> RoutingTreeRequestMatcher::serialize(const std::vector<JobRequest>& requests) {
    std::vector<uint8_t> packed;
    packed.push_back(COLL_ASSIGN_REQUESTS);
    for (auto& req : requests) {
        auto reqPacked = req.serialize();
        packed.insert(packed.end(), reqPacked.begin(), reqPacked.end());
    }
    return packed;
}

void RoutingTreeRequestMatcher::deserialize(const std::vector<uint8_t>& packed, int source) {

    int i = 0;
    uint8_t kind;
    int n = 1; memcpy(&kind, packed.data(), n); i += n;

    if (kind == COLL_ASSIGN_STATUS) {
        // Num idles + num cached per job
        
        int epoch;
        n = sizeof(int); memcpy(&epoch, packed.data()+i, n); i += n;
        if (epoch < _epoch) return; // obsolete!
        if (epoch > _epoch) {
            // new epoch
            _epoch = epoch;
            _tree.setEpoch(_epoch);
            _child_statuses.clear();
            _status_dirty = true;
        }

        Status status;
        memcpy(&status.numIdle, packed.data()+i, n); i += n;
        _child_statuses[source] = status;
        _status_dirty = true;

    } else if (kind == COLL_ASSIGN_REQUESTS) {
        // List of job requests
        size_t i = 1;
        while (i < packed.size()) {
            // Extract request
            std::vector<uint8_t> data(packed.data()+i, packed.data()+i+JobRequest::getMaxTransferSize());
            JobRequest req = Serializable::get<JobRequest>(data);
            // Accept or discard request
            if (req.balancingEpoch >= _epoch || req.requestedNodeIndex == 0) {
                req.numHops++;
                LOG_ADD_SRC(V4_VVER, "[CA] got %s", source, req.toStr().c_str());
                _request_list.insert(req);
            } else LOG_ADD_SRC(V4_VVER, "[CA] DISCARD %s", source, req.toStr().c_str());
            // Go to next request
            i += req.getTransferSize();
        }
    }
}

int RoutingTreeRequestMatcher::getDestination() {
    int destination = -1;
    // Is there an optimal fit for this request?
    // -- self?
    if (isIdle()) {
        // self is optimal fit
        destination = MyMpi::rank(MPI_COMM_WORLD);
    } else {
        // -- other PE? (choose at random)
        std::vector<int> viableDestinations;
        for (const auto& [rank, status] : _child_statuses) {
            if (status.numIdle > 0) {
                viableDestinations.push_back(rank);
            }
        }
        if (!viableDestinations.empty())
            destination = Random::choice(viableDestinations);
    }
    return destination;
}

void RoutingTreeRequestMatcher::resolveRequests() {

    if(_request_list.empty()) return;

    // Guard against recursive calls (would be illegal due to iteration over map)
    static bool resolving = false;
    assert(!resolving);
    resolving = true;

    std::vector<JobRequest> requestsToKeep;
    robin_hood::unordered_map<int, std::vector<JobRequest>> requestsPerDestination;

    // TODO if a request is digested locally but fails (e.g. because scheduler is busy),
    // it should not be added concurrently to the request list via addJobRequest.
    // It should be handled separately in some way, and there should be an explicit "retry"
    // as soon as the scheduler is not busy any longer.

    for (const auto& req : _request_list) {
        if (req.balancingEpoch < _epoch && req.requestedNodeIndex > 0) {
            // Obsolete request: Discard
            continue;
        }
        int id = req.jobId;
        int destination = getDestination();
        if (destination < 0) {
            // No fit found
            if (_tree.getCurrentRoot() == MyMpi::rank(MPI_COMM_WORLD)) {
                // I am the current root node: Keep request.
                requestsToKeep.push_back(req);
            } else {
                // Send job request upwards
                LOG_ADD_DEST(V5_DEBG, "[CA] Send %s to parent", _tree.getCurrentParent(), req.toStr().c_str());
                requestsPerDestination[_tree.getCurrentParent()].push_back(req);
            }
        } else {
            // Fit found: send to respective child
            // Update status
            if (destination == MyMpi::rank(MPI_COMM_WORLD)) {
                LOG(V4_VVER, "[CA] Digest %s locally\n", req.toStr().c_str());
                _local_request_callback(req, destination);
            } else {
                LOG_ADD_DEST(V4_VVER, "[CA] Send %s to dest.", destination, 
                    req.toStr().c_str());
                requestsPerDestination[destination].push_back(req);
                _child_statuses[destination].numIdle--;
            }
        }
    }

    // Re-insert requests which should be kept
    _request_list.clear();
    for (auto& req : requestsToKeep) _request_list.insert(std::move(req));
    // Send away requests for which some destination was found
    for (auto& [rank, requests] : requestsPerDestination) {
        auto packed = serialize(requests);
        MyMpi::isend(rank, MSG_NOTIFY_ASSIGNMENT_UPDATE, std::move(packed));
    }

    resolving = false;
}

void RoutingTreeRequestMatcher::addJobRequest(JobRequest& req) {
    if (req.balancingEpoch < _epoch && req.requestedNodeIndex > 0) return; // discard
    LOG(V5_DEBG, "[CA] Add req. %s\n", req.toStr().c_str());
    _request_list.insert(req);
}

RoutingTreeRequestMatcher::Status RoutingTreeRequestMatcher::getAggregatedStatus() {
    Status s;
    s.numIdle = isIdle() ? 1 : 0;
    for (auto& [childRank, childStatus] : _child_statuses) {
        s.numIdle += childStatus.numIdle;
    }
    return s;
}

void RoutingTreeRequestMatcher::advance(int epoch) {
    if (_job_registry == nullptr) return;
    bool newEpoch = epoch > _epoch;

    if (newEpoch) {
        _epoch = epoch;
        _tree.setEpoch(_epoch);
        _child_statuses.clear();
        _status_dirty = true;
    }
    
    resolveRequests();

    if (_status_dirty) {
        auto status = getAggregatedStatus();
        if (MyMpi::rank(MPI_COMM_WORLD) == _tree.getCurrentRoot()) {
            LOG(V3_VERB, "[CA] Root: %i requests, %i idle (epoch=%i)\n", _request_list.size(), status.numIdle, _epoch);
        } else {
            auto packedStatus = serialize(status);
            LOG_ADD_DEST(V5_DEBG, "[CA] Prop. status: %i idle (epoch=%i)", _tree.getCurrentParent(), status.numIdle, _epoch);
            MyMpi::isend(_tree.getCurrentParent(), MSG_NOTIFY_ASSIGNMENT_UPDATE, std::move(packedStatus));
        }
        _status_dirty = false;
    }
}

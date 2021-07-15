
#include "collective_assignment.hpp"

#include <assert.h>

#include "util/logger.hpp"
#include "data/job_database.hpp"

const uint8_t COLL_ASSIGN_STATUS = 1;
const uint8_t COLL_ASSIGN_REQUESTS = 2;

void CollectiveAssignment::handle(MessageHandle& handle) {
    deserialize(handle.getRecvData(), handle.source);
}

std::vector<uint8_t> CollectiveAssignment::serialize(const Status& status) {
    std::vector<uint8_t> packed(1 + 2*sizeof(int) + status.numCachedPerJob.size()*2*sizeof(int));
    int i = 0, n;
    n = 1; memcpy(packed.data() + i, &COLL_ASSIGN_STATUS, n); i += n;
    n = sizeof(int);
    memcpy(packed.data() + i, &_epoch, n); i += n;
    memcpy(packed.data() + i, &status.numIdle, n); i += n;
    for (auto& [jobId, numCached] : status.numCachedPerJob) {
        memcpy(packed.data()+i, &jobId, n); i += n;
        memcpy(packed.data()+i, &numCached, n); i += n;
    }
    return packed;
}

std::vector<uint8_t> CollectiveAssignment::serializeRequests() {
    std::vector<uint8_t> packed;
    packed.push_back(COLL_ASSIGN_REQUESTS);
    for (auto& req : _request_list) {
        auto reqPacked = req.serialize();
        packed.insert(packed.end(), reqPacked.begin(), reqPacked.end());
    }
    return packed;
}

void CollectiveAssignment::deserialize(const std::vector<uint8_t>& packed, int source) {

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
            _child_statuses.clear();
            _status_dirty = true;
        }

        Status status;
        memcpy(&status.numIdle, packed.data()+i, n); i += n;
        while (i+2*sizeof(int) <= packed.size()) {
            int jobId, numCached;
            memcpy(&jobId, packed.data()+i, n); i += n;
            memcpy(&numCached, packed.data()+i, n); i += n;
            status.numCachedPerJob[jobId] = numCached;
        }
        _child_statuses[source] = status;
        _status_dirty = true;

    } else if (kind == COLL_ASSIGN_REQUESTS) {
        // List of job requests

        while (i+sizeof(JobRequest) <= packed.size()) {
            std::vector<uint8_t> reqPacked(packed.begin()+i, packed.begin()+i+sizeof(JobRequest));
            _request_list.insert(Serializable::get<JobRequest>(reqPacked));
            i += sizeof(JobRequest);
        }
    }
}

void CollectiveAssignment::resolveRequests() {
    
    std::vector<JobRequest> requestsToKeep;

    for (const auto& req : _request_list) {
        int id = req.jobId;
        int destination = -1;
        bool usesCachedSlot = false;

        // Is there an optimal fit for this request?
        // -- self?
        if (_job_db->isIdle() && _job_db->hasDormantJob(id)) {
            // self is optimal fit
            destination = MyMpi::rank(MPI_COMM_WORLD);
        } else {
            // -- other PE?
            int bestNumCached = 0;
            for (const auto& [rank, status] : _child_statuses) {
                if (status.numIdle > 0 && status.numCachedPerJob.count(id) && 
                    status.numCachedPerJob.at(id) > bestNumCached) {
                    destination = rank;
                    bestNumCached = status.numCachedPerJob.at(id);
                    usesCachedSlot = true;
                }
            }
        }

        if (destination < 0) {
            // No optimal fit found

            // Is there a non-optimal fit for this request?
            // -- self?
            if (_job_db->isIdle()) destination = MyMpi::rank(MPI_COMM_WORLD);
            else {
                // -- other PE?
                for (const auto& [rank, status] : _child_statuses) {
                    if (status.numIdle > 0) {
                        destination = rank;
                        break;
                    }
                }
            }
        }

        if (destination < 0) {
            // No fit found
            if (getCurrentRoot() == MyMpi::rank(MPI_COMM_WORLD)) {
                // I am the current root node: Keep request.
                requestsToKeep.push_back(req);
            } else {
                // Send job request upwards
                log(LOG_ADD_DESTRANK | V3_VERB, "[CA] Send %s to parent", getCurrentParent(), req.toStr().c_str());
                MyMpi::isend(MPI_COMM_WORLD, getCurrentParent(), MSG_REQUEST_NODE, req);
            }
        } else {
            // Fit found: send to respective child
            log(LOG_ADD_DESTRANK | V3_VERB, "[CA] Send %s to destination", destination, req.toStr().c_str());
            MyMpi::isend(MPI_COMM_WORLD, destination, MSG_REQUEST_NODE, req);
            // Update status
            if (destination != MyMpi::rank(MPI_COMM_WORLD)) {
                _child_statuses[destination].numIdle--;
                if (usesCachedSlot)
                    _child_statuses[destination].numCachedPerJob[id]--;
            }
        }
    }

    _request_list.clear();
    for (auto& req : requestsToKeep) _request_list.insert(std::move(req));
}

void CollectiveAssignment::setStatusDirty() {
    _status_dirty = true;
}

void CollectiveAssignment::addJobRequest(JobRequest& req) {
    log(V3_VERB, "[CA] Add req. %s\n", req.toStr().c_str());
    _request_list.insert(req);
}

CollectiveAssignment::Status CollectiveAssignment::getAggregatedStatus() {
    Status s;
    s.numIdle = _job_db->isIdle() ? 1 : 0;
    for (int jobId : _job_db->getDormantJobs()) s.numCachedPerJob[jobId] = 1;
    for (auto& [childRank, childStatus] : _child_statuses) {
        s.numIdle += childStatus.numIdle;
        for (auto& [jobId, numCached] : childStatus.numCachedPerJob)
            s.numCachedPerJob[jobId] += numCached;
    }
    return s;
}

void CollectiveAssignment::advance(int epoch) {
    bool newEpoch = epoch > _epoch;

    if (newEpoch) {
        _epoch = epoch;
        _child_statuses.clear();
        _status_dirty = true;
    }

    if (_num_workers <= 1) return;
    
    resolveRequests();

    auto status = getAggregatedStatus();
    if (_status_dirty) {
        if (MyMpi::rank(MPI_COMM_WORLD) == getCurrentRoot()) {
            log(V3_VERB, "[CA] Root: %i requests, %i idle (epoch=%i)\n", _request_list.size(), status.numIdle, _epoch);
        } else {
            auto packedStatus = serialize(status);
            log(LOG_ADD_DESTRANK | V3_VERB, "[CA] Prop. status: %i idle (epoch=%i)", getCurrentParent(), status.numIdle, _epoch);
            MyMpi::isend(MPI_COMM_WORLD, getCurrentParent(), MSG_NOTIFY_ASSIGNMENT_UPDATE, packedStatus);
        }
        _status_dirty = false;
    }
}

int CollectiveAssignment::getCurrentRoot() {
    assert(_num_workers > 0);
    return robin_hood::hash<int>()(_epoch) % _num_workers;
}

int CollectiveAssignment::getCurrentParent() {
    return _neighbor_towards_rank[getCurrentRoot()];
}

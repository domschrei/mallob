
#pragma once

#include <list>
#include <map>
#include <optional>
#include "util/robin_hood.hpp"
#include "data/job_transfer.hpp"
#include "comm/mympi.hpp"
#include "balancing/request_matcher.hpp"
#include "data/worker_sysstate.hpp"
#include "comm/randomized_routing_tree.hpp"

typedef std::function<void(JobRequest& req, int source)> DeflectJobRequestCallback;

class RequestManager {

private:
    Parameters& _params;
    WorkerSysState& _sys_state;
    RandomizedRoutingTree& _routing_tree;

    // Requests which lay dormant (e.g., due to too many hops / too busy system)
    // and will be re-introduced to continue hopping after some time
    std::list<std::tuple<float, int, JobRequest>> _deferred_requests;
    std::map<int, std::list<MessageHandle>> _future_request_msgs;
    robin_hood::unordered_map<int, std::list<JobRequest>> _root_requests;

    // Request to re-activate a local dormant root
    std::optional<JobRequest> _pending_root_reactivate_request;

    RequestMatcher* _req_matcher;

public:
    RequestManager(Parameters& params, WorkerSysState& sysstate, RandomizedRoutingTree& tree, RequestMatcher* reqMatcher) 
        : _params(params), _sys_state(sysstate), _routing_tree(tree), _req_matcher(reqMatcher) {}

    void onNoRequestEmitted(Job& job, bool left) {
        // trigger destruction mechanism of the job request to multiply
        // such that message notifications are sent to awaiting ranks
        auto& optReqToMultiply = job.getRequestToMultiply(left);
        optReqToMultiply.reset();
    }

    enum DiscardCallbackExtent {LEFT, RIGHT, BOTH};
    void installDiscardCallback(JobRequest& req, DiscardCallbackExtent extent) {
        req.setMultiplicityDiscardCallback([&, extent](JobRequest& r) {
            // send notification to direct child(ren)
            auto [leftReq, rightReq] = r.getMultipliedChildRequests(-1);
            if (extent != RIGHT && leftReq.jobId != -1) {
                LOG(V4_VVER, "CANCEL %s\n", leftReq.toStr().c_str());
                MyMpi::isend(leftReq.multiBegin % _routing_tree.getNumWorkers(), 
                    MSG_MATCHING_SEND_REQUEST_OBSOLETE_NOTIFICATION, leftReq);
            }
            if (extent != LEFT && rightReq.jobId != -1) {
                LOG(V4_VVER, "CANCEL %s\n", rightReq.toStr().c_str());
                MyMpi::isend(rightReq.multiBegin % _routing_tree.getNumWorkers(), 
                    MSG_MATCHING_SEND_REQUEST_OBSOLETE_NOTIFICATION, rightReq);
            }
        });
    }

    void spawnJobRequest(Job& job, bool left, int balancingEpoch) {
        auto req = job.spawnJobRequest(left, balancingEpoch);
        int tag = MSG_REQUEST_NODE;
        emitJobRequest(job, req, tag, left, -1);
    }

    void emitJobRequest(Job& job, JobRequest& req, int tag, bool left, int dest) {

        // undirected request?
        if (tag == MSG_REQUEST_NODE || dest == -1) {
            
            // check if the job has a request which must be multiplied
            auto& optReqToMultiply = job.getRequestToMultiply(left);
            if (optReqToMultiply) {

                // -- yes: replace input request with multiplied request
                auto& reqToMultiply = optReqToMultiply.value();
                tag = MSG_MATCHING_SEND_REQUEST;
                auto [reqLeft, reqRight] = reqToMultiply.getMultipliedChildRequests(MyMpi::rank(MPI_COMM_WORLD));
                req = left ? reqLeft : reqRight;
                dest = req.multiBegin % _routing_tree.getNumWorkers();
                LOG(V4_VVER, "multiplying request %s --\n", reqToMultiply.toStr().c_str());
                LOG_ADD_DEST(V4_VVER, "-- becomes %s", dest, req.toStr().c_str());

                reqToMultiply.dismissMultiplicityData(); // promise fulfilled
                optReqToMultiply.reset();

            } else if (_params.bulkRequests()) {
                // Which transitive children *below* the requested index does this job desire?
                // Perform a depth-first search as far as the job's volume allows.
                addMultiplicityToRequest(req, job.getVolume());
            }
        }

        // Find a proper destination rank if none was given
        if (dest == -1) {
            int nextNodeRank = job.getJobTree().getRankOfNextDormantChild(); 
            if (nextNodeRank < 0) {
                tag = MSG_REQUEST_NODE;
                nextNodeRank = left ? job.getJobTree().getLeftChildNodeRank() : job.getJobTree().getRightChildNodeRank();
            }
            dest = nextNodeRank;
        }

        LOG_ADD_DEST(V3_VERB, "%s growing: %s", dest, job.toStr(), req.toStr().c_str());
        _sys_state.addLocal(SYSSTATE_SPAWNEDREQUESTS, 1);
        auto time = Timer::elapsedSeconds();
        if (left) job.getJobTree().setDesireLeft(time);
        else job.getJobTree().setDesireRight(time);

        if (tag == MSG_REQUEST_NODE && _params.prefixSumMatching() && _params.bulkRequests()) {
            _req_matcher->addJobRequest(req);
            return;
        }
        
        MyMpi::isend(dest, tag, req);
    }

    void activateRootRequest(int jobId) {
        LOG(V5_DEBG, "Try activate #%i\n", jobId);
        auto optReq = getRootRequest(jobId);
        if (!optReq.has_value()) return;
        LOG(V3_VERB, "Activate %s\n", optReq.value().toStr().c_str());
        deflectJobRequest(optReq.value(), optReq.value().requestingNodeRank);
    }

    void deflectJobRequest(JobRequest& request, int senderRank) {

        // Increment #hops
        request.numHops++;
        int num = request.numHops;
        _sys_state.addLocal(SYSSTATE_NUMHOPS, 1);

        // Show warning if #hops is a large power of two
        if ((num >= 512) && ((num & (num - 1)) == 0)) {
            LOG(V1_WARN, "[WARN] %s\n", request.toStr().c_str());
        }

        // If hopped enough for collective assignment to be enabled
        // and if either reactivation scheduling is employed or the requested node is non-root
        if (_req_matcher && (num >= _params.hopsUntilCollectiveAssignment())
            && (_params.reactivationScheduling() || request.requestedNodeIndex > 0)) {

            request.triggerAndDestroyMultiplicityData();
            if (_params.prefixSumMatching() && _params.bulkRequests()) {
                // Reset multiplicity according to multiEnd-multiBegin. Example:
                // 0.206 0 PRISMA contribute r.#3596:3 rev. 0 <- [0] born=0.203 hops=1 epoch=5 x4 [1,4]
                // -- effectively looking for 4-1=3 workers
                // ... prefix sum calculation ...
                // 0.207 7 PRISMA received r.#3596:3 rev. 0 <- [0] born=0.203 hops=1 epoch=5 x4 [7,11]
                // -- now requesting 11-7=4 workers!
                // => Danger of increasing the desired interval!
                if (request.multiplicity > 1 && request.multiBegin >= 0 && request.multiEnd >= 0) {
                    request.multiplicity = request.multiEnd - request.multiBegin;
                    request.multiBegin = -1;
                    request.multiEnd = -1;
                }
            }
            _req_matcher->addJobRequest(request);
            return;
        }

        // Get random choice from bounce alternatives
        int nextRank = _routing_tree.getRandomNeighbor();
        if (_routing_tree.getNumNeighbors() > 2) {
            // ... if possible while skipping the requesting node and the sender
            while (nextRank == request.requestingNodeRank || nextRank == senderRank) {
                nextRank = _routing_tree.getRandomNeighbor();
            }
        }

        // Send request to "next" worker node
        LOG_ADD_DEST(V5_DEBG, "Hop %s", nextRank, Job::toStr(request.jobId, request.requestedNodeIndex).c_str());
        MyMpi::isend(nextRank, MSG_REQUEST_NODE, request);
    }

    void defer(JobRequest&& req, int sender) {
        LOG(V3_VERB, "Defer %s\n", req.toStr().c_str());
        _deferred_requests.emplace_back(Timer::elapsedSecondsCached(), sender, std::move(req));
    }

    void forwardDeferredRequests() {
    
        std::vector<std::pair<JobRequest, int>> result;
        for (auto& [deferredTime, senderRank, req] : _deferred_requests) {
            if (Timer::elapsedSecondsCached() - deferredTime < 1.0f) break;
            result.emplace_back(std::move(req), senderRank);
            LOG(V3_VERB, "Reactivate deferred %s\n", req.toStr().c_str());
        }

        for (size_t i = 0; i < result.size(); i++) {
            _deferred_requests.pop_front();
        }

        for (auto& [req, senderRank] : result) {
            deflectJobRequest(req, senderRank);
        }
    }

    bool hasPendingRootReactivationRequest() const {
        return _pending_root_reactivate_request.has_value();
    }

    JobRequest loadPendingRootReactivationRequest() {
        assert(hasPendingRootReactivationRequest());
        JobRequest r = _pending_root_reactivate_request.value();
        _pending_root_reactivate_request.reset();
        return r;
    }

    void setPendingRootReactivationRequest(JobRequest&& req) {
        assert(!hasPendingRootReactivationRequest() 
            || req.jobId == _pending_root_reactivate_request.value().jobId);
        _pending_root_reactivate_request = std::move(req);
    }

    void addFutureRequestMessage(int epoch, MessageHandle&& h) {
        _future_request_msgs[epoch].push_back(std::move(h));
    }

    std::list<MessageHandle> getArrivedFutureRequests(int presentEpoch) {
        std::list<MessageHandle> out;
        auto it = _future_request_msgs.begin();
        while (it != _future_request_msgs.end()) {
            auto& [epoch, msgs] = *it;
            if (epoch <= presentEpoch) {
                // found a candidate message
                assert(!msgs.empty());
                out.splice(out.end(), std::move(msgs));
                it = _future_request_msgs.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

    void addRootRequest(const JobRequest& req) {
        _root_requests[req.jobId].push_back(req);
        LOG(V5_DEBG, "added root request %s\n", _root_requests[req.jobId].back().toStr().c_str());
    }

    std::optional<JobRequest> getRootRequest(int jobId) {
        
        auto it = _root_requests.find(jobId);
        if (it == _root_requests.end()) return std::optional<JobRequest>();

        auto& list = it->second;
        std::optional<JobRequest> optReq = std::optional<JobRequest>(list.front());
        list.pop_front();
        if (list.empty()) _root_requests.erase(jobId);
        return optReq;
    }

    std::optional<MessageHandle> tryGetPendingRootActivationRequest() {

        if (!hasPendingRootReactivationRequest()) return std::optional<MessageHandle>();

        MessageHandle handle;
        handle.tag = MSG_REQUEST_NODE;
        handle.receiveSelfMessage(loadPendingRootReactivationRequest().serialize(), MyMpi::rank(MPI_COMM_WORLD));
        return handle;
    }

    void addMultiplicityToRequest(JobRequest& req, int jobVolume) {
        req.multiplicity = getCurrentDesiredRequestMultiplicity(jobVolume, req.requestedNodeIndex);
    }

private:
    int getCurrentDesiredRequestMultiplicity(int volume, int index) {

        // Which transitive children *below* the requested index does this job desire?
        // Perform a depth-first search as far as the job's volume allows.
        std::vector<int> indexStack(1, index);
        int multiplicity = 0;
        while (!indexStack.empty()) {
            multiplicity++;
            
            // Visit node
            int index = indexStack.back();
            indexStack.pop_back();

            // Expand children
            for (auto childIndex : {2*index+1, 2*index+2}) {
                if (childIndex < volume) {
                    indexStack.push_back(childIndex);
                }
            }
        }
        return multiplicity;
    }

};

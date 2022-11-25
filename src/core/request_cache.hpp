
#pragma once

#include <list>
#include <map>
#include <optional>
#include "util/robin_hood.hpp"
#include "data/job_transfer.hpp"
#include "comm/mympi.hpp"

typedef std::function<void(JobRequest& req, int source)> DeflectJobRequestCallback;

class RequestCache {

private:
    // Requests which lay dormant (e.g., due to too many hops / too busy system)
    // and will be re-introduced to continue hopping after some time
    std::list<std::tuple<float, int, JobRequest>> _deferred_requests;
    std::map<int, std::list<MessageHandle>> _future_request_msgs;
    robin_hood::unordered_map<int, std::list<JobRequest>> _root_requests;

    // Request to re-activate a local dormant root
    std::optional<JobRequest> _pending_root_reactivate_request;

public:
    RequestCache() {}

    void defer(const JobRequest& req, int sender) {
        LOG(V3_VERB, "Defer %s\n", req.toStr().c_str());
        _deferred_requests.emplace_back(Timer::elapsedSecondsCached(), sender, req);
    }

    void forwardDeferredRequests(DeflectJobRequestCallback cb) {
    
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
            cb(req, senderRank);
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
        handle.finished = true;
        handle.receiveSelfMessage(loadPendingRootReactivationRequest().serialize(), MyMpi::rank(MPI_COMM_WORLD));
        return handle;
    }

};

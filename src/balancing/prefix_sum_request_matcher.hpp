
#pragma once

#include "request_matcher.hpp"
#include "comm/async_collective.hpp"
#include "data/job_transfer.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "util/hashing.hpp"
#include "util/tsl/robin_map.h"

class PrefixSumRequestMatcher : public RequestMatcher {

private:
    const int COLLECTIVE_ID_SPARSE_PREFIX_SUM = 401627;
    const int CALL_ID_REQUESTS = 1;
    const int CALL_ID_IDLES = 2;

    MPI_Comm _comm;
    AsyncCollective<ReduceableInt> _collective;
    int _my_rank;

    bool _last_contribution_idle {false};
    std::list<JobRequest> _new_requests;
    std::list<JobRequest> _requests_in_prefix_sum;

    int _requests_indexing_offset {0};
    std::list<std::pair<std::pair<int, int>, JobRequest>> _indexed_requests;
    int _last_requests_total {0};
    int _last_requests_matched {0};

    int _idles_indexing_offset {0};
    std::list<int> _idles_indexes;
    int _last_idles_total {0};
    int _last_idles_matched {0};

    struct Matching {
        int idleRank {-1};
        bool requestArrived {false};
        JobRequest request;
        std::string toStr() const {
            return "{" + std::to_string(idleRank) + "} x {" + 
                std::string(!requestArrived ? "-" : (request.jobId == -1 ? "X" : request.toStr())) 
                + "}";
        }
    };
    tsl::robin_map<int, Matching> _open_matchings;
    int _running_matching_id {0};

    float _time_of_last_change_in_matching {0};

    std::list<MessageSubscription> _subscriptions;

    struct BroadcastEvent {
        int callType;
        int id;
        int exclusiveSum;
        int inclusiveSum;
        int totalSum;
        bool operator<(const BroadcastEvent& other) const {
            return id < other.id;
        }
    };
    std::set<BroadcastEvent> _events;
    int _last_event_id {0};

public:
    PrefixSumRequestMatcher(JobRegistry& jobRegistry, MPI_Comm comm, 
            std::function<void(const JobRequest&, int)> localRequestCallback) :
        _comm(comm), RequestMatcher(jobRegistry, comm, localRequestCallback),
        _collective(comm, MyMpi::getMessageQueue(), COLLECTIVE_ID_SPARSE_PREFIX_SUM) {

        _collective.initializeSparsePrefixSum(CALL_ID_REQUESTS, 
                /*delaySeconds=*/0.001, [&](auto& results) {
            auto it = results.begin();
            int excl = it->content; ++it;
            int incl = it->content; ++it;
            int total = it->content; ++it;
            int id = _collective.getNumReceivedResults();
            LOG(V5_DEBG, "PRISMA recv prefix sum X%i - requests (%i,%i,%i)\n", id, excl, incl, total);
            _events.insert(BroadcastEvent{CALL_ID_REQUESTS, id, excl, incl, total});
        });
        _collective.initializeSparsePrefixSum(CALL_ID_IDLES, 
                /*delaySeconds=*/0.001, [&](auto& results) {
            auto it = results.begin();
            int excl = it->content; ++it;
            int incl = it->content; ++it;
            int total = it->content; ++it;
            int id = _collective.getNumReceivedResults();
            LOG(V5_DEBG, "PRISMA recv prefix sum X%i - idles (%i,%i,%i)\n", id, excl, incl, total);
            _events.insert(BroadcastEvent{CALL_ID_IDLES, id, excl, incl, total});
        });

        auto callback = [&](auto& h) {handle(h);};
        for (int tag : {MSG_MATCHING_SEND_IDLE_TOKEN, MSG_MATCHING_SEND_REQUEST, 
                MSG_MATCHING_SEND_REQUEST_OBSOLETE_NOTIFICATION, MSG_MATCHING_REQUEST_CANCELLED}) {
            _subscriptions.emplace_back(tag, callback);
        }
        
        _my_rank = MyMpi::rank(comm);
    }

    virtual void addJobRequest(JobRequest& request) override {
        _new_requests.push_back(request);
    }

    virtual void handle(MessageHandle& h) override {

        if (h.tag == MSG_MATCHING_SEND_IDLE_TOKEN) {
            auto vec = Serializable::get<IntVec>(h.getRecvData());
            int id = vec[0];
            int idleRank = vec[1];
            _open_matchings[id].idleRank = idleRank;
            LOG(V4_VVER, "PRISMA id=%i received idle [%i]\n", id, idleRank);
            tryResolve(id);
        }
        if (h.tag == MSG_MATCHING_SEND_REQUEST) {
            auto req = Serializable::get<JobRequest>(h.getRecvData());
            int id = req.getMatchingId();
            _open_matchings[id].request = req;
            _open_matchings[id].requestArrived = true;
            LOG(V4_VVER, "PRISMA id=%i received %s\n", id, req.toStr().c_str());
            tryResolve(id);
        }
        if (h.tag == MSG_MATCHING_SEND_REQUEST_OBSOLETE_NOTIFICATION) {
            // Absorb an idle token for this request, ignore the request
            auto req = Serializable::get<JobRequest>(h.getRecvData());
            int id = req.getMatchingId();
            LOG(V4_VVER, "PRISMA id=%i received cancelled %s\n", id, req.toStr().c_str());
            _open_matchings[id].requestArrived = true;
            // Propagate multiplied child requests as well
            auto [leftReq, rightReq] = req.getMultipliedChildRequests(-1);
            for (auto& childReq : {leftReq, rightReq}) {
                if (childReq.jobId == -1) continue;
                LOG(V4_VVER, "PRISMA CANCEL %s\n", childReq.toStr().c_str());
                MyMpi::isend(childReq.multiBegin % MyMpi::size(_comm), 
                    MSG_MATCHING_SEND_REQUEST_OBSOLETE_NOTIFICATION, childReq);
            }
            tryResolve(id);
        }
        if (h.tag == MSG_MATCHING_REQUEST_CANCELLED) {
            // A request which THIS IDLE NODE should have received
            // has been cancelled. Re-participate in the idles prefix sum.
            if (isIdle()) _status_dirty = true;
        }

        _time_of_last_change_in_matching = Timer::elapsedSecondsCached();
    }

    virtual void advance(int epoch) override {

        if (_job_registry == nullptr) return;
        _epoch = std::max(_epoch, epoch);

        auto idle = isIdle();
        // Report idle status only if your status was marked dirty 
        if (idle && _status_dirty) {
            LOG(V5_DEBG, "PRISMA contribute idle status\n");
            _collective.contributeToSparsePrefixSum(CALL_ID_IDLES, ReduceableInt(1));
            _status_dirty = false;
        }

        if (!_new_requests.empty()) { // TODO wait until ready to contribute *again*
            for (auto& req : _new_requests) LOG(V4_VVER, "PRISMA contribute %s\n", req.toStr().c_str());
            // Contribute to the requests prefix sum
            _collective.contributeToSparsePrefixSum(CALL_ID_REQUESTS, getSumOfNewRequestsMultiplicities());
            // Move requests from "new" to "in prefix sum"
            _requests_in_prefix_sum.splice(_requests_in_prefix_sum.end(), _new_requests);
        }

        _collective.advanceSparseOperations();

        if (!doneMatching()) tryMatch();
        if (!doneMatching()) return; // something still missing

        // Process events in the correct order
        auto it = _events.begin();
        while (it != _events.end()) {
            auto& event = *it;

            if (event.id > _last_event_id+1) {
                // Consecutive event to the last processed event did not arrive yet
                break;
            }

            // Process event
            if (event.callType == CALL_ID_REQUESTS) {
                LOG(V5_DEBG, "PRISMA indexed %i requests - (%i,%i,%i)\n", event.totalSum, 
                    event.exclusiveSum, event.inclusiveSum, event.totalSum);
                digestRequestsPrefixSumResult(event.exclusiveSum, event.inclusiveSum, event.totalSum);
            }
            if (event.callType == CALL_ID_IDLES) {
                LOG(V5_DEBG, "PRISMA indexed %i idles - (%i,%i,%i)\n", event.totalSum, 
                    event.exclusiveSum, event.inclusiveSum, event.totalSum);
                digestIdlesPrefixSumResult(event.exclusiveSum, event.inclusiveSum, event.totalSum);
            }
            tryMatch();

            _last_event_id = event.id;
            it = _events.erase(it);
        }

        // Output requests and/or idles which have not been matched for at least a second
        if (Timer::elapsedSecondsCached() - _time_of_last_change_in_matching >= 1.0f) {
            for (auto& [id, matching] : _open_matchings) {
                LOG(V4_VVER, "PRISMA open matching id=%i %s\n", id, matching.toStr().c_str());
            }
            _time_of_last_change_in_matching = Timer::elapsedSecondsCached();
        }
    }

    virtual void setStatusDirty(StatusDirtyReason reason) override {
        if (!isIdle()) return;
        switch (reason) {
        case DISCARD_REQUEST:
        case REJECT_REQUEST:
        case STOP_WAIT_FOR_REACTIVATION:
        case UNCOMMIT_JOB_LEAVING:
        case BECOME_IDLE:
            _status_dirty = true;
        }
    }

private:

    int getSumOfNewRequestsMultiplicities() const {
        int sum = 0;
        for (auto& req : _new_requests) {
            sum += req.multiplicity;
        }
        return sum;
    }

    void tryResolve(int id) {
        auto& matching = _open_matchings[id];
        if (matching.requestArrived && matching.idleRank != -1) {

            if (matching.request.jobId == -1) {
                // request cancelled
                LOG(V3_VERB, "PRISMA EMIT [%i] <= (cancelled)\n", matching.idleRank);
                MyMpi::isend(matching.idleRank, MSG_MATCHING_REQUEST_CANCELLED, IntVec{0});
            } else {
                LOG(V3_VERB, "PRISMA id=%i EMIT [%i] <= %s\n", id, matching.idleRank, 
                    matching.request.toStr().c_str());
                MyMpi::isend(matching.idleRank, MSG_REQUEST_NODE, matching.request);
            }

            _open_matchings.erase(id);
        }
    }

    void tryMatch() {

        //skipMatchingStepsWithoutLocalContribution();

        // Use the indexed requests and the most recent prefix sum result.
        while (!doneMatching()) {

            // A request and an idle rank can be matched!
            int idlePrefixSumIndex = _idles_indexing_offset + _last_idles_matched;
            int requestPrefixSumIndex = _requests_indexing_offset + _last_requests_matched;
            int destinationRank = _running_matching_id % MyMpi::size(_comm);

            bool hadLocalContribution = false;

            if (!_indexed_requests.empty()) {
                auto& [indexStart, indexEnd] = _indexed_requests.front().first;
                if (requestPrefixSumIndex >= indexStart && requestPrefixSumIndex < indexEnd) {
                    // THIS request is the one to be matched
                    auto& req = _indexed_requests.front().second;

                    // Set correct multiplicity range for this request
                    req.multiBegin = destinationRank;
                    req.multiEnd = destinationRank + req.multiplicity;
                    req.multiBaseId = _running_matching_id;

                    // only send the first "incarnation" of a request with multiplicity > 1
                    if (requestPrefixSumIndex == indexStart) {
                        MyMpi::isend(destinationRank, MSG_MATCHING_SEND_REQUEST, req);
                        LOG(V4_VVER, "PRISMA id=%i MATCH I%i =>[%i]<= Q%i (%s)\n", 
                            _running_matching_id, idlePrefixSumIndex, destinationRank, 
                            requestPrefixSumIndex, req.toStr().c_str());
                    }
                    hadLocalContribution = true;

                    // Pop this request from indexed request structure
                    // if it is the last "incarnation"
                    if (requestPrefixSumIndex+1 == indexEnd) {
                        _indexed_requests.pop_front();
                    }
                }
            }

            if (!_idles_indexes.empty()) {
                int idleIndex = _idles_indexes.front();
                if (idleIndex == requestPrefixSumIndex) {
                    // THIS idle rank is the one to be matched
                    _idles_indexes.pop_front();

                    // send this rank
                    IntVec idleVec({_running_matching_id, _my_rank});
                    MyMpi::isend(destinationRank, MSG_MATCHING_SEND_IDLE_TOKEN, idleVec);
                    hadLocalContribution = true;
                    LOG(V4_VVER, "PRISMA id=%i MATCH I%i [%i] =>[%i]<= Q%i\n", 
                        _running_matching_id, idlePrefixSumIndex, _my_rank, 
                        destinationRank, requestPrefixSumIndex);
                }
            }

            //assert(hadLocalContribution);

            // Go to next step
            _last_idles_matched++;
            _last_requests_matched++;
            _running_matching_id++;

            // skip any non-local steps
            //skipMatchingStepsWithoutLocalContribution();
        }
    }

    bool doneMatching() const {
        return _last_idles_matched >= _last_idles_total || _last_requests_matched >= _last_requests_total;
    }

    /*
    void skipMatchingStepsWithoutLocalContribution() {

        // How many steps until there is a local relevant request?        
        bool hasRequestContribution = !_indexed_requests.empty();
        int skippableStepsForRequests = hasRequestContribution ? 
            _indexed_requests.front().first.first - (_requests_indexing_offset + _last_requests_matched) 
            : std::numeric_limits<int>::max();
        skippableStepsForRequests = std::max(0, skippableStepsForRequests);
        skippableStepsForRequests = std::min(skippableStepsForRequests, _last_requests_total-_last_requests_matched);

        // How many steps until there is a local relevant idle rank?
        bool hasIdleContribution = _last_idles_excl < _last_idles_incl;
        int skippableStepsForIdles = hasIdleContribution && _last_idles_matched < _last_idles_incl ?
            std::max(_last_idles_matched, _last_idles_excl) - _last_idles_matched
            : std::numeric_limits<int>::max();
        assert(skippableStepsForIdles >= 0);
        skippableStepsForIdles = std::min(skippableStepsForIdles, _last_idles_total-_last_idles_matched);

        // How many steps can be skipped overall?
        int skippableSteps = std::min(skippableStepsForIdles, skippableStepsForRequests);

        // Skip.
        _last_idles_matched += skippableSteps;
        _last_requests_matched += skippableSteps;
        _running_matching_id += skippableSteps;
    }
    */

    void digestIdlesPrefixSumResult(int exclusiveSum, int inclusiveSum, int totalSum) {

        // Integrate the remaining idles from the last prefix sum
        // into these new idle ranks.
        int numRemainingIdles = _last_idles_total - _last_idles_matched;
        assert(numRemainingIdles >= 0);

        _idles_indexing_offset += _last_idles_total;

        if (inclusiveSum != exclusiveSum) {
            // your own contribution is part of the prefix sum
            assert(inclusiveSum - exclusiveSum == 1);
            int index = _idles_indexing_offset + exclusiveSum;
            LOG(V4_VVER, "PRISMA indexed I%i\n", index);
            _idles_indexes.push_back(index);
        }

        _last_idles_total = totalSum;
        // make sure to still match old remaining idles!
        _last_idles_matched = - numRemainingIdles;
    }

    void digestRequestsPrefixSumResult(int exclusiveSum, int inclusiveSum, int totalSum) {

        // Integrate the remaining requests from the last prefix sum
        // into these new requests.
        int numRemainingRequests = _last_requests_total - _last_requests_matched;
        assert(numRemainingRequests >= 0);

        _requests_indexing_offset += _last_requests_total;
        
        auto reqIt = _requests_in_prefix_sum.begin();
        int index = exclusiveSum;
        while (index < inclusiveSum) {
            assert(reqIt != _requests_in_prefix_sum.end());
            
            // Extract job request
            JobRequest req = std::move(*reqIt);
            reqIt = _requests_in_prefix_sum.erase(reqIt);

            auto reqIndex = _requests_indexing_offset + index;
            auto reqIndexEnd = reqIndex + req.multiplicity;
            LOG(V4_VVER, "PRISMA indexed [Q%i..Q%i) : %s\n", reqIndex, reqIndexEnd, req.toStr().c_str());
            _indexed_requests.emplace_back(std::pair<int, int>(reqIndex, reqIndexEnd), std::move(req));

            index += req.multiplicity;
        }

        _last_requests_total = totalSum;
        // make sure to still match old remaining requests!
        _last_requests_matched = - numRemainingRequests;
    }
};

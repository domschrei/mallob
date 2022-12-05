
#pragma once

#include "request_matcher.hpp"
#include "comm/async_collective.hpp"
#include "data/job_transfer.hpp"
#include "comm/message_subscription.hpp"

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

    //int _idles_indexing_offset {0};
    int _last_idles_incl {0};
    int _last_idles_excl {0};
    int _last_idles_total {0};
    int _last_idles_matched {0};

    std::list<JobRequest> _requests_to_match;
    std::list<int> _idles_to_match;
    int _num_cancelled_requests_to_match {0};
    int _running_rank_to_perform_match {0};

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
        _collective.initializeDifferentialSparsePrefixSum(CALL_ID_IDLES, 
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
            _idles_to_match.push_back(Serializable::get<IntVec>(h.getRecvData())[0]);
            LOG(V4_VVER, "PRISMA received idle [%i]\n", _idles_to_match.back());
        }
        if (h.tag == MSG_MATCHING_SEND_REQUEST) {
            _requests_to_match.push_back(Serializable::get<JobRequest>(h.getRecvData()));
            LOG(V4_VVER, "PRISMA received %s\n", _requests_to_match.back().toStr().c_str());
        }
        if (h.tag == MSG_MATCHING_SEND_REQUEST_OBSOLETE_NOTIFICATION) {
            // Absorb an idle token for this request, ignore the request
            auto req = Serializable::get<JobRequest>(h.getRecvData());
            LOG(V4_VVER, "PRISMA received cancelled %s\n", req.toStr().c_str());
            _num_cancelled_requests_to_match++;
            // Propagate multiplied child requests as well
            auto [leftReq, rightReq] = req.getMultipliedChildRequests(-1);
            for (auto& childReq : {leftReq, rightReq}) {
                if (childReq.jobId == -1) continue;
                LOG(V4_VVER, "CANCEL %s\n", childReq.toStr().c_str());
                MyMpi::isend(childReq.multiBegin % MyMpi::size(_comm), 
                    MSG_MATCHING_SEND_REQUEST_OBSOLETE_NOTIFICATION, childReq);
            }
        }
        if (h.tag == MSG_MATCHING_REQUEST_CANCELLED) {
            // A request which THIS IDLE NODE should have received
            // has been cancelled. Re-participate in the idles prefix sum.
            if (isIdle()) _status_dirty = true;
        }

        // Perform matching of arrived requests and idles for as long as possible
        while (!_idles_to_match.empty() && !_requests_to_match.empty()) {
            int idleRank = _idles_to_match.front();
            _idles_to_match.pop_front();
            auto request = std::move(_requests_to_match.front()); 
            _requests_to_match.pop_front();

            LOG(V3_VERB, "PRISMA EMIT [%i] <= %s\n", idleRank, request.toStr().c_str());
            MyMpi::isend(idleRank, MSG_REQUEST_NODE, request);
        }
        // Idles are also matched with cancelled requests
        while (!_idles_to_match.empty() && _num_cancelled_requests_to_match > 0) {
            int idleRank = _idles_to_match.front();
            _idles_to_match.pop_front();
            _num_cancelled_requests_to_match--;
            LOG(V3_VERB, "PRISMA EMIT [%i] <= (cancelled)\n", idleRank);
            MyMpi::isend(idleRank, MSG_MATCHING_REQUEST_CANCELLED, IntVec{0});
        }

        _time_of_last_change_in_matching = Timer::elapsedSecondsCached();
    }

    virtual void advance(int epoch) override {

        if (_job_registry == nullptr) return;
        _epoch = std::max(_epoch, epoch);

        auto idle = isIdle();
        if ((idle != _last_contribution_idle) || _status_dirty) {
            LOG(V5_DEBG, "PRISMA contribute idle status\n");
            _collective.contributeToSparsePrefixSum(CALL_ID_IDLES, ReduceableInt(isIdle()?1:0));
            _last_contribution_idle = idle;
            _status_dirty = false;
        }

        if (!_new_requests.empty()) { // TODO wait until ready to contribute *again*
            for (auto& req : _new_requests) LOG(V4_VVER, "PRISMA contribute %s\n", req.toStr().c_str());
            // Contribute to the requests prefix sum
            _collective.contributeToSparsePrefixSum(CALL_ID_REQUESTS, getSumOfNewRequestsMultiplicities());
            // Move requests from "new" to "in prefix sum"
            _requests_in_prefix_sum.splice(_requests_in_prefix_sum.end(), _new_requests);
        }

        _collective.advanceSparseOperations(Timer::elapsedSeconds());

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
                //_idles_indexing_offset += _last_idles_total;
                _last_idles_excl = event.exclusiveSum;
                _last_idles_incl = event.inclusiveSum;
                _last_idles_total = event.totalSum;
                _last_idles_matched = 0;
                LOG(V5_DEBG, "PRISMA indexed %i idles - (%i,%i,%i)\n", _last_idles_total, 
                    event.exclusiveSum, event.inclusiveSum, event.totalSum);
            }
            tryMatch();

            _last_event_id = event.id;
            it = _events.erase(it);
        }

        // Output requests and/or idles which have not been matched for at least a second
        if (Timer::elapsedSecondsCached() - _time_of_last_change_in_matching >= 1.0f) {
            if (!_idles_to_match.empty()) {
                LOG(V1_WARN, "[WARN] PRISMA %i idles seem orphaned\n", _idles_to_match.size());
            }
            if (!_requests_to_match.empty() || _num_cancelled_requests_to_match > 0) {
                LOG(V1_WARN, "[WARN] PRISMA %i requests (%i proper, %i cancelled) seem orphaned\n", 
                    _requests_to_match.size() + _num_cancelled_requests_to_match,
                    _requests_to_match.size(), _num_cancelled_requests_to_match);
            }
            _time_of_last_change_in_matching = Timer::elapsedSecondsCached();
        }
    }

    virtual void setStatusDirty(StatusDirtyReason reason) override {
        if (isIdle() && (reason == DISCARD_REQUEST || reason == REJECT_REQUEST)) {
            // confirm that you are still idle indeed
            _status_dirty = true;
        }
        // other reasons are caught by changes in the return value of isIdle()
    }

private:

    int getSumOfNewRequestsMultiplicities() const {
        int sum = 0;
        for (auto& req : _new_requests) {
            sum += req.multiplicity;
        }
        return sum;
    }

    void tryMatch() {

        //skipMatchingStepsWithoutLocalContribution();

        // Use the indexed requests and the most recent prefix sum result.
        while (!doneMatching()) {

            // A request and an idle rank can be matched!
            int idlePrefixSumIndex = _last_idles_matched;
            int requestPrefixSumIndex = _requests_indexing_offset + _last_requests_matched;
            int destinationRank = _running_rank_to_perform_match % MyMpi::size(_comm);

            bool hadLocalContribution = false;

            if (!_indexed_requests.empty()) {
                auto& [indexStart, indexEnd] = _indexed_requests.front().first;
                if (requestPrefixSumIndex >= indexStart && requestPrefixSumIndex < indexEnd) {
                    // THIS request is the one to be matched
                    auto& req = _indexed_requests.front().second;

                    // Set correct multiplicity range for this request
                    req.multiBegin = destinationRank;
                    req.multiEnd = destinationRank + req.multiplicity;

                    // only send the first "incarnation" of a request with multiplicity > 1
                    if (requestPrefixSumIndex == indexStart) {
                        MyMpi::isend(destinationRank, MSG_MATCHING_SEND_REQUEST, req);
                        LOG(V4_VVER, "PRISMA MATCH I%i =>[%i]<= Q%i (%s)\n", 
                            idlePrefixSumIndex, destinationRank, requestPrefixSumIndex,
                            req.toStr().c_str());
                    }
                    hadLocalContribution = true;

                    // Pop this request from indexed request structure
                    // if it is the last "incarnation"
                    if (requestPrefixSumIndex+1 == indexEnd) {
                        _indexed_requests.pop_front();
                    }
                }
            }

            if (_last_idles_matched >= _last_idles_excl && _last_idles_matched < _last_idles_incl) {
                // THIS rank is the idle rank that should be matched

                // send this rank
                IntVec idleVec({_my_rank});
                MyMpi::isend(destinationRank, MSG_MATCHING_SEND_IDLE_TOKEN, idleVec);
                hadLocalContribution = true;
                LOG(V4_VVER, "PRISMA MATCH I%i [%i] =>[%i]<= Q%i\n", 
                    idlePrefixSumIndex, _my_rank, destinationRank, requestPrefixSumIndex);
            }

            //assert(hadLocalContribution);

            // Go to next step
            _last_idles_matched++;
            _last_requests_matched++;
            _running_rank_to_perform_match++;

            // skip any non-local steps
            //skipMatchingStepsWithoutLocalContribution();
        }
    }

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
        _running_rank_to_perform_match += skippableSteps;
    }

    bool doneMatching() const {
        return _last_idles_matched >= _last_idles_total || _last_requests_matched >= _last_requests_total;
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

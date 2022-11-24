
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

    std::list<JobRequest> _new_requests;
    std::list<JobRequest> _requests_in_prefix_sum;

    int _requests_indexing_offset {0};
    std::list<std::pair<int, JobRequest>> _indexed_requests;
    int _last_requests_total {0};
    int _last_requests_matched {0};

    //int _idles_indexing_offset {0};
    int _last_idles_incl {0};
    int _last_idles_excl {0};
    int _last_idles_total {0};
    int _last_idles_matched {0};

    std::list<JobRequest> _requests_to_match;
    std::list<int> _idles_to_match;
    int _running_rank_to_perform_match {0};

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
    PrefixSumRequestMatcher(JobDatabase& jobDb, MPI_Comm comm, 
            std::function<void(const JobRequest&, int)> localRequestCallback) :
        _comm(comm), RequestMatcher(jobDb, comm, localRequestCallback),
        _collective(comm, MyMpi::getMessageQueue(), COLLECTIVE_ID_SPARSE_PREFIX_SUM) {

        _collective.initializeSparsePrefixSum(CALL_ID_REQUESTS, 
                /*delaySeconds=*/0.001, [&](auto& results) {
            auto it = results.begin();
            int excl = it->content; ++it;
            int incl = it->content; ++it;
            int total = it->content; ++it;
            int id = _collective.getNumReceivedResults();
            LOG(V5_DEBG, "PRISMA recv prefix sum #%i - requests (%i,%i,%i)\n", id, excl, incl, total);
            _events.insert(BroadcastEvent{CALL_ID_REQUESTS, id, excl, incl, total});
        });
        _collective.initializeDifferentialSparsePrefixSum(CALL_ID_IDLES, 
                /*delaySeconds=*/0.001, [&](auto& results) {
            auto it = results.begin();
            int excl = it->content; ++it;
            int incl = it->content; ++it;
            int total = it->content; ++it;
            int id = _collective.getNumReceivedResults();
            LOG(V5_DEBG, "PRISMA recv prefix sum #%i - idles (%i,%i,%i)\n", id, excl, incl, total);
            _events.insert(BroadcastEvent{CALL_ID_IDLES, id, excl, incl, total});
        });

        _subscriptions.emplace_back(MSG_MATCHING_SEND_IDLE_TOKEN, [&](auto& h) {handle(h);});
        _subscriptions.emplace_back(MSG_MATCHING_SEND_REQUEST, [&](auto& h) {handle(h);});
        
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

        while (!_idles_to_match.empty() && !_requests_to_match.empty()) {
            int idleRank = _idles_to_match.front();
            _idles_to_match.pop_front();
            auto request = std::move(_requests_to_match.front()); 
            _requests_to_match.pop_front();

            MyMpi::isend(idleRank, MSG_REQUEST_NODE, request);
        }
    }

    virtual void advance(int epoch) override {

        if (_job_db == nullptr) return;
        bool newEpoch = epoch > _epoch;

        if (newEpoch) {
            _epoch = epoch;
            _status_dirty = true;
        }

        if (_status_dirty) {
            LOG(V5_DEBG, "PRISMA contribute idle status\n");
            _collective.contributeToSparsePrefixSum(CALL_ID_IDLES, ReduceableInt(isIdle()?1:0));
            _status_dirty = false;
        }

        if (!_new_requests.empty()) { // TODO wait until ready to contribute *again*
            for (auto& req : _new_requests) LOG(V4_VVER, "PRISMA contribute %s\n", req.toStr().c_str());
            // Contribute to the requests prefix sum
            _collective.contributeToSparsePrefixSum(CALL_ID_REQUESTS, _new_requests.size());
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
    }

private:

    void tryMatch() {

        // TODO Skip indices where you are not involved. Something like this:
        // int stepsUntilLocalIdle = _last_idles_excl > _last_idles_matched ? _last_idles_excl - _last_idles_matched : 0;
        // int stepsUntilLocalReq = std::max(0, _indexed_requests.empty() ? MyMpi::size(_comm) : _indexed_requests.front().first - _last_requests_matched);

        // Use the indexed requests and the most recent prefix sum result.
        while (!doneMatching()) {
            // A request and an idle rank can be matched!

            int idlePrefixSumIndex = _last_idles_matched;
            int requestPrefixSumIndex = _requests_indexing_offset + _last_requests_matched;
            int destinationRank = _running_rank_to_perform_match % MyMpi::size(_comm);

            LOG(V4_VVER, "PRISMA matching idle #%i and request #%i to rank %i\n", 
                idlePrefixSumIndex, requestPrefixSumIndex, destinationRank);

            if (!_indexed_requests.empty() && _indexed_requests.front().first == requestPrefixSumIndex) {
                // THIS request is the one to be matched
                auto [index, req] = _indexed_requests.front();
                _indexed_requests.pop_front();

                // send request
                MyMpi::isend(destinationRank, MSG_MATCHING_SEND_REQUEST, req);
            }

            if (_last_idles_matched >= _last_idles_excl && _last_idles_matched < _last_idles_incl) {
                // THIS rank is the idle rank that should be matched

                // send this rank
                IntVec idleVec({_my_rank});
                MyMpi::isend(destinationRank, MSG_MATCHING_SEND_IDLE_TOKEN, idleVec);
            }

            _last_idles_matched++;
            _last_requests_matched++;
            _running_rank_to_perform_match++;
        }
    }

    bool doneMatching() const {
        return _last_idles_matched >= _last_idles_total || _last_requests_matched >= _last_requests_total;
    }

    void digestRequestsPrefixSumResult(int exclusiveSum, int inclusiveSum, int totalSum) {

        _requests_indexing_offset += _last_requests_total;
        
        auto reqIt = _requests_in_prefix_sum.begin();
        for (int index = exclusiveSum; index < inclusiveSum; index++) {
            assert(reqIt != _requests_in_prefix_sum.end());
            
            // Extract job request
            JobRequest req = std::move(*reqIt);
            reqIt = _requests_in_prefix_sum.erase(reqIt);

            LOG(V4_VVER, "PRISMA indexed %s\n", req.toStr().c_str());
            _indexed_requests.emplace_back(_requests_indexing_offset + index, std::move(req));
        }

        _last_requests_total = totalSum;
        _last_requests_matched = 0;
    }
};

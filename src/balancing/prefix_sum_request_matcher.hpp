
#pragma once

#include "request_matcher.hpp"
#include "comm/async_collective.hpp"
#include "data/job_transfer.hpp"

class PrefixSumRequestMatcher : public RequestMatcher {

private:
    struct PrefixSumElement : public Reduceable {
        
        int numIdleTokens {0};
        int numRequests {0};

        virtual std::vector<uint8_t> serialize() const override {
            std::vector<uint8_t> packed(2*sizeof(int));
            size_t i = 0;
            memcpy(packed.data()+i, &numIdleTokens, sizeof(int)); i += sizeof(int);
            memcpy(packed.data()+i, &numRequests, sizeof(int)); i += sizeof(int);
            return packed;
        }
        virtual PrefixSumElement& deserialize(const std::vector<uint8_t>& packed) override {
            size_t i = 0;
            memcpy(&numIdleTokens, packed.data()+i, sizeof(int)); i += sizeof(int);
            memcpy(&numRequests, packed.data()+i, sizeof(int)); i += sizeof(int);
            return *this;
        }
        virtual bool isEmpty() const override {
            return numIdleTokens == 0 && numRequests == 0;
        }
        virtual void aggregate(const Reduceable& other) override {
            PrefixSumElement* otherElem = (PrefixSumElement*) &other;
            numIdleTokens += otherElem->numIdleTokens;
            numRequests += otherElem->numRequests;
        }
        bool operator==(const PrefixSumElement& other) const {
            return numIdleTokens == other.numIdleTokens && numRequests == other.numRequests;
        }
        bool operator!=(const PrefixSumElement& other) const {
            return !(*this == other);
        }
    };
    const int COLLECTIVE_ID_SPARSE_PREFIX_SUM = 1;
    const int CALL_ID_SPARSE_PREFIX_SUM = 1;
    AsyncCollective<PrefixSumElement> _collective;
    int _my_rank;

    std::list<JobRequest> _new_requests;
    std::list<JobRequest> _requests_in_prefix_sum;
    std::list<JobRequest> _requests_to_match;
    std::set<int> _new_idles;
    std::set<int> _idles_in_prefix_sum;
    std::set<int> _idles_to_match;

    PrefixSumElement _last_contributed_elem;
    int _last_contributed_epoch {-1};

    MessageQueue::CallbackRef _cb_ref_idle_token;
    MessageQueue::CallbackRef _cb_ref_request;

public:
    PrefixSumRequestMatcher(JobDatabase& jobDb, MPI_Comm comm, 
            std::function<void(const JobRequest&, int)> localRequestCallback) :
        RequestMatcher(jobDb, comm, localRequestCallback),
        _collective(comm, MyMpi::getMessageQueue(), COLLECTIVE_ID_SPARSE_PREFIX_SUM) {

        _collective.initializeSparsePrefixSum(CALL_ID_SPARSE_PREFIX_SUM, 
            /*delaySeconds=*/0.001, [&](auto& results) {
            auto it = results.begin();
            PrefixSumElement& excl = *it; ++it;
            PrefixSumElement& incl = *it; ++it;
            PrefixSumElement& total = *it; ++it;
            digestPrefixSumResult(excl, incl, total);
        });

        _cb_ref_idle_token = MyMpi::getMessageQueue().registerCallback(MSG_MATCHING_SEND_IDLE_TOKEN, 
            [&](auto& h) {handle(h);});
        _cb_ref_request = MyMpi::getMessageQueue().registerCallback(MSG_MATCHING_SEND_REQUEST, 
            [&](auto& h) {handle(h);});
        
        _my_rank = MyMpi::rank(comm);
    }
    virtual ~PrefixSumRequestMatcher() {
        MyMpi::getMessageQueue().clearCallback(MSG_MATCHING_SEND_IDLE_TOKEN, _cb_ref_idle_token);
        MyMpi::getMessageQueue().clearCallback(MSG_MATCHING_SEND_REQUEST, _cb_ref_request);
    }

    virtual void addJobRequest(JobRequest& request) override {
        _new_requests.push_back(request);
    }

    virtual void handle(MessageHandle& h) override {

        if (h.tag == MSG_MATCHING_SEND_IDLE_TOKEN) {
            _idles_to_match.push_back(Serializable::get<IntVec>(h.getRecvData())[0]);
        }
        if (h.tag == MSG_MATCHING_SEND_REQUEST) {
            _requests_to_match.push_back(Serializable::get<JobRequest>(h.getRecvData()));
        }

        while (!_idles_to_match.empty() && !_requests_to_match.empty()) {
            auto idleRank = _idles_to_match.front(); 
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

            if (isIdle()) _new_idles.insert(_my_rank);

            PrefixSumElement elem;
            elem.numIdleTokens = _new_idles.size();
            elem.numRequests = _new_requests.size();
            
            if (elem.numRequests > 0 || elem.numIdleTokens != _last_contributed_elem.numIdleTokens 
                    || _epoch != _last_contributed_epoch) {
                // Contribute to the prefix sum
                _collective.contributeToSparsePrefixSum(CALL_ID_SPARSE_PREFIX_SUM, elem);
                _last_contributed_elem = elem;
                _last_contributed_epoch = _epoch;
                // Move requests from "new" to "in prefix sum"
                _requests_in_prefix_sum.splice(_requests_in_prefix_sum.end(), _new_requests);
                _idles_in_prefix_sum.insert(_new_idles.begin(), _new_idles.end());
                _new_idles.clear();
            }

            _status_dirty = false;
        }

        _collective.advanceSparseOperations(Timer::elapsedSeconds());
    }

private:
    void digestPrefixSumResult(const PrefixSumElement& exclusiveSum, const PrefixSumElement& inclusiveSum, 
            const PrefixSumElement& totalSum) {

        LOG(V3_VERB, "PRISMA digest prefix sum result\n");

        // If your idle token index surpasses the TOTAL number of requests in this prefix sum,
        // then do not send the idle token, but make sure to retry in the next prefix sum.
        // If a request index surpasses the TOTAL number of idle tokens in this prefix sum,
        // then do not send the request but defer it to the next prefix sum.

        // Forward idle tokens
        std::list<int> deferredIdles;
        auto idleIt = _idles_in_prefix_sum.begin();
        for (int dest = exclusiveSum.numIdleTokens; dest < inclusiveSum.numIdleTokens; dest++) {
            assert(idleIt != _idles_in_prefix_sum.end());

            // Extract idle token
            int token = *idleIt;
            idleIt = _idles_in_prefix_sum.erase(idleIt);
            
            if (dest >= totalSum.numRequests) {
                // No request available for this idle token!
                // Make sure to retry in the next prefix sum.
                setStatusDirty();
                break;
            }
            IntVec token({MyMpi::rank(MPI_COMM_WORLD)});
            MyMpi::isend(dest, MSG_MATCHING_SEND_IDLE_TOKEN, token);
        }

        // Forward requests
        std::list<JobRequest> deferredRequests;
        auto reqIt = _requests_in_prefix_sum.begin();
        for (int dest = exclusiveSum.numRequests; dest < inclusiveSum.numRequests; dest++) {
            assert(reqIt != _requests_in_prefix_sum.end());
            
            // Extract job request
            JobRequest req = std::move(*reqIt);
            reqIt = _requests_in_prefix_sum.erase(reqIt);
            
            if (dest >= totalSum.numIdleTokens) {
                // No idle token available for this request!
                // Move request back to the incoming requests queue.
                deferredRequests.push_back(std::move(req));
                continue;
            }

            // Forward request to destination
            MyMpi::isend(dest, MSG_MATCHING_SEND_REQUEST, req);
        }
        // Move requests from "deferred" to "new"
        _new_requests.splice(_new_requests.begin(), deferredRequests);
    }
};

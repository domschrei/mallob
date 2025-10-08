
#pragma once

#include <atomic>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "data/checksum.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"

class SatJobStreamProcessor {

public:
    struct SatTask {
        enum Type {RAW, SPLIT} type {SPLIT};
        std::vector<int> lits;
        std::vector<int> assumptions;
        std::string descLabel;
        float priority;
        Checksum chksum;
        int rev {-1};
        void integrate(const SatTask& other) {
            integrate(SatTask(other));
        }
        void integrate(SatTask&& other) {
            assert(other.rev != rev);
            if (other.rev > rev) {
                rev = other.rev;
                type = other.type;
                descLabel = std::move(other.descLabel);
                assumptions = std::move(other.assumptions);
                priority = other.priority;
                // TODO handle checksum
                chksum = other.chksum;
            }
            if (lits.empty()) lits = std::move(other.lits);
            else {
                if (type == RAW) {
                    // Truncate away old fingerprint and assumptions
                    assert(lits.back() == INT32_MIN);
                    lits.resize(lits.size() - 1 - SIG_SIZE_BYTES/sizeof(int));
                    while (lits.back() != INT32_MAX) lits.pop_back();
                    lits.pop_back();
                    assert(lits.back() == 0);
                }
                // Append new clauses, fingerprint, and assumptions
                for (int lit : other.lits) lits.push_back(lit);
            }
        }
    };
    struct SatTaskResult {
        int resultCode {0};
        std::vector<int> solution;
    };
    struct Synchronizer {
        std::atomic_int lastEndedRev {-1};
        SPSCBlockingRingbuffer<SatTaskResult> resultQueue {8};
        bool concludeRevision(int revToEnd, int resultCode, std::vector<int>&& solution) {
            int expectedPriorRev = revToEnd-1;
            bool ok = lastEndedRev.compare_exchange_strong(expectedPriorRev, revToEnd, std::memory_order_relaxed);
            if (!ok) return false;
            assert(resultQueue.empty());
            SatTaskResult res {resultCode, std::move(solution)};
            ok = resultQueue.pushBlocking(res);
            return ok;
        }
    };

protected:
    std::string _name;
    std::function<bool(int)> _terminator;
    std::function<const SatTask&()> _cb_retrieve_full_task;

private:
    Synchronizer& _sync;
    SPSCBlockingRingbuffer<SatTask> _queue;

public:
    SatJobStreamProcessor(Synchronizer& sync) : _sync(sync), _queue(8'192) {}
    virtual ~SatJobStreamProcessor() {}

    virtual void setName(const std::string& baseName) {
        _name = baseName + ":base";
    }
    virtual void setRetrieveFullTaskCallback(std::function<const SatTask&()> cb) {
        _cb_retrieve_full_task = cb;
    }
    virtual void setTerminator(const std::function<bool(int)>& terminator) {
        _terminator = terminator;
    }

    virtual void process(SatTask& task) = 0;

    virtual void finalize() {
        LOG(V2_INFO, "%s finalize\n", _name.c_str());
        _queue.markExhausted();
        _queue.markTerminated();
    }

    void submit(const SatTask& task) {
        submit(SatTask(task));
    }
    void submit(SatTask&& task) {
        //SatTask task {revision, newLiterals, assumptions, "", 1, {}};
        _queue.pushBlocking(task);
    }

    SPSCBlockingRingbuffer<SatTask>& getQueue() {
        return _queue;
    }

protected:
    bool concludeRevision(int revToEnd, int resultCode, std::vector<int>&& solution) {
        return _sync.concludeRevision(revToEnd, resultCode, std::move(solution));
    }
};

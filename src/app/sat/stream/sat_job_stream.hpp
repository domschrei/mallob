
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "sat_job_stream_processor.hpp"
#include "data/checksum.hpp"
#include "util/assert.hpp"
#include "util/sys/background_worker.hpp"

class SatJobStream {

private:
    const std::string _name;
    int _active_rev {-1};
    SatJobStreamProcessor::Synchronizer _sync;
    typedef std::pair<std::unique_ptr<SatJobStreamProcessor>, std::unique_ptr<BackgroundWorker>> Processor;
    std::vector<Processor> _processors;
    volatile bool _finalizing {false};
    volatile bool _idle {true};

public:
    SatJobStream(const std::string& baseName) : _name(baseName) {}
    ~SatJobStream() {
        interrupt();
        finalize();
    }

    SatJobStreamProcessor::Synchronizer& getSynchronizer() {
        return _sync;
    }
    // Transfers ownership of the pointer.
    void addProcessor(SatJobStreamProcessor* processor) {
        processor->setName(_name);
        Processor p {std::unique_ptr<SatJobStreamProcessor>(processor), std::unique_ptr<BackgroundWorker>(new BackgroundWorker())};
        p.second->run([&, processor = p.first.get(), bgWorker = p.second.get()]() {
            SatJobStreamProcessor::SatTask accumulatedTask;
            while (bgWorker->continueRunning()) {
                SatJobStreamProcessor::SatTask task;
                bool ok = processor->getQueue().pollBlocking(task);
                if (!ok) break;

                accumulatedTask.integrate(task);
                if (_sync.lastEndedRev.load(std::memory_order_relaxed) >= accumulatedTask.rev) {
                    continue;
                }

                processor->process(accumulatedTask); // blocking

                accumulatedTask = SatJobStreamProcessor::SatTask();
            }
        });
        _processors.push_back(std::move(p));
    }

    std::pair<int, std::vector<int>> solve(std::vector<int>&& newLiterals, const std::vector<int>& assumptions,
            const std::string& descriptionLabel = "", float priority = 0, Checksum chksum = {}) {

        solveNonblocking(std::move(newLiterals), assumptions, descriptionLabel, priority, chksum);
        return getNonblockingSolveResult();
    }

    void solveNonblocking(std::vector<int>&& newLiterals, const std::vector<int>& assumptions,
            const std::string& descriptionLabel = "", float priority = 0, Checksum chksum = {}) {

        assert(newLiterals.empty() || newLiterals.front() != 0);
        assert(newLiterals.empty() || newLiterals.back() == 0);

        _active_rev++;
        assert(_sync.resultQueue.empty());

        for (auto& [proc, worker] : _processors) {
            std::vector<int> litsCopy = newLiterals;
            proc->submit(_active_rev, std::move(litsCopy), assumptions);
        }
        _idle = false;
    }
    bool isIdle() const {
        return _idle;
    }
    bool isNonblockingSolvePending() {
        return _sync.resultQueue.empty();
    }
    std::pair<int, std::vector<int>> getNonblockingSolveResult() {
        SatJobStreamProcessor::SatTaskResult result;
        bool ok = _sync.resultQueue.pollBlocking(result);
        assert(ok);
        _idle = true;
        return {result.resultCode, std::move(result.solution)};
    }

    void setTerminator(const std::function<bool()>& terminator) {
        for (auto& [proc, worker] : _processors)
            proc->setTerminator([&, terminator](int rev) {
                if (terminator() || rev <= _sync.lastEndedRev.load(std::memory_order_relaxed)) {
                    _sync.concludeRevision(rev, 0, {});
                    return true;
                }
                return false;
            });
    }

    bool interrupt() {
        bool success = _sync.concludeRevision(_active_rev, 0, {});
        _idle = true;
        return success;
    }

    void finalize() {
        _finalizing = true;
        for (auto& [proc, worker] : _processors) proc->finalize();
    }

    bool finalizing() const {
        return _finalizing;
    }
};

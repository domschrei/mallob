
#pragma once

#include <atomic>
#include <fstream>
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

    SatJobStreamProcessor::SatTask _complete_task;
    bool _initialized_complete_task {false};

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
        processor->setRetrieveFullTaskCallback([this]() -> const SatJobStreamProcessor::SatTask& {return _complete_task;});
        Processor p {std::unique_ptr<SatJobStreamProcessor>(processor), std::unique_ptr<BackgroundWorker>(new BackgroundWorker())};
        p.second->run([&, processor = p.first.get(), bgWorker = p.second.get()]() {
            SatJobStreamProcessor::SatTask accumulatedTask;
            bool taskInitialized {false};
            while (bgWorker->continueRunning()) {
                SatJobStreamProcessor::SatTask task;
                bool ok = processor->getQueue().pollBlocking(task);
                if (!ok) break;

                if (!taskInitialized) {
                    accumulatedTask.type = task.type;
                    taskInitialized = true;
                }
                accumulatedTask.integrate(std::move(task));
                if (_sync.lastEndedRev.load(std::memory_order_relaxed) >= accumulatedTask.rev) {
                    continue;
                }

                processor->process(accumulatedTask); // blocking

                accumulatedTask = SatJobStreamProcessor::SatTask{accumulatedTask.type};
            }
        });
        _processors.push_back(std::move(p));
    }

    std::pair<int, std::vector<int>> solve(SatJobStreamProcessor::SatTask&& task) {
        solveNonblocking(std::move(task));
        return getNonblockingSolveResult();
    }

    void solveNonblocking(SatJobStreamProcessor::SatTask&& task) {

        assert(task.lits.empty() || task.lits.front() != 0);

        _active_rev++;
        assert(_sync.resultQueue.empty());
        task.rev = _active_rev;

        for (auto& [proc, worker] : _processors) {
            proc->submit(task);
        }
        if (!_initialized_complete_task) {
            _complete_task.type = task.type;
            _initialized_complete_task = true;
        }
        _complete_task.integrate(std::move(task));
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
    int getRevision() const {return _active_rev;}

    void setTerminator(const std::function<bool()>& terminator) {
        for (auto& [proc, worker] : _processors)
            proc->setTerminator([&, terminator](int rev) {
                if ((terminator && terminator()) || rev <= _sync.lastEndedRev.load(std::memory_order_relaxed)) {
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


#pragma once

#include <assert.h>
#include <limits>
#include <algorithm>
#include <iterator>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app/sat/data/clause.hpp"
#include "app/sat/data/produced_clause_candidate.hpp"
#include "app/sat/sharing/filter/exact_clause_filter.hpp"
#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/sharing/store/adaptive_clause_store.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "comm/msgtags.h"
#include "data/job_transfer.hpp"
#include "util/params.hpp"
#include "base_sat_job.hpp"
#include "util/logger.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/threading.hpp"
#include "app/job_tree.hpp"
#include "app/sat/sharing/buffer/buffer_builder.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "util/sys/background_worker.hpp"

bool areIntervalsOverlapping(int begin1, int end1, int begin2, int end2);

class HistoricClauseStorage {

private:
    struct StorageDiagnostics {
        int numLitsInStorage;
        int numClausesInStorage;
        std::string slotLayout;
    };

    struct BackgroundTask {
        enum Type {INSERT, INSERT_AND_IMPORT, REQUEST} type;
        int epochBegin;
        int epochEnd;
        bool fromLeftChild {false};
        std::vector<int> clauses;
        bool finished {false};
        StorageDiagnostics storageDiagnostics;

        BackgroundTask(Type type, int epochBegin, int epochEnd, const std::vector<int>& clauses, bool fromLeftChild) : 
            type(type), epochBegin(epochBegin), epochEnd(epochEnd), 
            fromLeftChild(fromLeftChild), clauses(clauses) {}
        BackgroundTask(Type type, int epochBegin, int epochEnd, std::vector<int>&& clauses, bool fromLeftChild) : 
            type(type), epochBegin(epochBegin), epochEnd(epochEnd), 
            fromLeftChild(fromLeftChild), clauses(std::move(clauses)) {}
    };

    class Worker {
    
    private:
        AdaptiveClauseStore::Setup _setup;
        int _max_same_breadth_slots {3};
        // storage
        struct Slot {
            int epochBegin;
            int epochEnd;
            AdaptiveClauseStore cdb;
            Slot(int epochBegin, int epochEnd, AdaptiveClauseStore::Setup setup) : 
                epochBegin(epochBegin), epochEnd(epochEnd), cdb(setup) {}
        };
        std::list<Slot> _storage_list;
        std::unique_ptr<GenericClauseStore> _filter_store; // really only a dummy required for the filter
        std::unique_ptr<ExactClauseFilter> _filter;

        ConditionVariable _cond_var_bg_tasks;
        Mutex _mtx_bg_tasks;
        std::list<BackgroundTask> _bg_tasks;
        BackgroundWorker _bg_worker;

        int _num_open_tasks {0};

    public:
        Worker(const AdaptiveClauseStore::Setup& setup) : _setup(setup),
                _filter_store(new AdaptiveClauseStore(_setup)),
                _filter(new ExactClauseFilter(*_filter_store, std::numeric_limits<int>::max(), _setup.maxEffectiveClauseLength)) {
            _bg_worker.run([this]() {runBackgroundWorker();});
        }

        void addTask(BackgroundTask::Type type, int epochBegin, int epochEnd, std::vector<int>&& clauses, bool fromLeftChild) {
            {
                auto lock = _mtx_bg_tasks.getLock();
                _bg_tasks.emplace_back(type, epochBegin, epochEnd, std::move(clauses), fromLeftChild);
            }
            _cond_var_bg_tasks.notify();
            _num_open_tasks++;
        }

        bool hasFinishedTask() {
            if (_num_open_tasks == 0) return false;
            auto lock = _mtx_bg_tasks.getLock();
            if (_bg_tasks.empty()) return false;
            return _bg_tasks.front().finished;
        }

        BackgroundTask getFinishedTask() {
            auto lock = _mtx_bg_tasks.getLock();
            BackgroundTask task = std::move(_bg_tasks.front());
            _bg_tasks.pop_front();
            lock.unlock();
            _cond_var_bg_tasks.notify();
            _num_open_tasks--;
            return task;
        }

        ~Worker() {
            {
                auto lock = _mtx_bg_tasks.getLock();
                _bg_worker.stopWithoutWaiting();
            }
            _cond_var_bg_tasks.notify();
        }

    private:
        void runBackgroundWorker() {

            while (_bg_worker.continueRunning()) {

                _cond_var_bg_tasks.wait(_mtx_bg_tasks, 
                    [&]() {
                        return !_bg_worker.continueRunning() || 
                            (!_bg_tasks.empty() && !_bg_tasks.back().finished);
                    }
                );

                // Try to find an unprocessed task
                auto lock = _mtx_bg_tasks.getLock();
                auto it = _bg_tasks.begin();
                while (it != _bg_tasks.end() && it->finished) ++it; 
                if (it == _bg_tasks.end()) continue;
                BackgroundTask& task = *it;
                lock.unlock();

                if (task.type == BackgroundTask::REQUEST) {
                    processRequestTask(task);
                } else {
                    processInsertTask(task);
                }
                addStorageDiagnosticsToTask(task);
                task.finished = true;
            }
        }

        void processRequestTask(BackgroundTask& task) {

            for (auto& slot : _storage_list) {
                if (areIntervalsOverlapping(slot.epochBegin, slot.epochEnd, task.epochBegin, task.epochEnd)) {
                    // Pretend the task is for this exact slot
                    task.epochBegin = slot.epochBegin;
                    task.epochEnd = slot.epochEnd;
                    // Read clauses from the slot into the task's clauses
                    int numExported;
                    task.clauses = slot.cdb.readBuffer();
                    return;
                }
            }
        }

        void processInsertTask(BackgroundTask& task) {

            if (task.type == BackgroundTask::INSERT_AND_IMPORT) {
                // This is a retroactively sent "missing interval"
                
                // -- integrate into a present storage slot
                bool success = false;
                for (auto& slot : _storage_list) {
                    if (areIntervalsOverlapping(slot.epochBegin, slot.epochEnd, task.epochBegin, task.epochEnd)) {
                        auto [accepted, total] = addClausesIntoDatabase(slot.cdb, task.clauses, 
                            task.epochBegin, ClauseAdditionMode::INSERT_AND_IMPORT);
                        // The clauses in task.clauses are now the ones which HAVE been inserted successfully,
                        // i.e., the ones which were not contained in the storage yet
                        success = true;
                        break;
                    }
                }
                if (!success) task.clauses.clear(); // no clauses to import to the solver

            } else {
                // This is a normal interval to append
                assert(task.epochEnd - task.epochBegin == 1);

                // Create new storage slot of breadth 1
                _storage_list.emplace_back(task.epochBegin, task.epochBegin+1, _setup);
                AdaptiveClauseStore& cdb = _storage_list.back().cdb;

                // Insert non-duplicate clauses into new storage slot as well as lookup table
                auto [accepted, total] = addClausesIntoDatabase(cdb, task.clauses, 
                    task.epochBegin, ClauseAdditionMode::INSERT);
                //LOG(V4_VVER, "Added %i/%i cls to HCS\n", accepted, total);

                // Repair invariant of having at most _max_same_breadth_slots slots of the same breadth.
                mergeSlots();
            }
        }

        void addStorageDiagnosticsToTask(BackgroundTask& task) {

            int numStoredLits = 0;
            for (auto& slot : _storage_list) {
                numStoredLits += slot.cdb.getCurrentlyUsedLiterals();
            }
            task.storageDiagnostics.numClausesInStorage = _filter->size(0);
            task.storageDiagnostics.numLitsInStorage = numStoredLits;
            if (_storage_list.back().epochEnd < 80)
                task.storageDiagnostics.slotLayout = reportSlots();
            else
                task.storageDiagnostics.slotLayout = "...";
        }

        void mergeSlots() {

            if (_storage_list.empty()) return;

            //LOG(V4_VVER, "HCS SLOTS %s\n", reportSlots().c_str());

            auto it = _storage_list.end(); --it;
            int lastBreadth = 1;
            int numSlotsThisBreadth = 0;
            while (true) {
                auto& slot = *it;
                //LOG(V4_VVER, "HCS - [%i,%i)\n", slot.epochBegin, slot.epochEnd);

                int slotBreadth = slot.epochEnd - slot.epochBegin;
                // New breadth reached?
                if (slotBreadth != lastBreadth) {
                    lastBreadth = slotBreadth;
                    numSlotsThisBreadth = 0;
                }

                // How many slots of this breadth did I find yet?
                numSlotsThisBreadth++;
                if (numSlotsThisBreadth <= _max_same_breadth_slots) {
                    // all ok
                    // finish or go to next
                    if (it == _storage_list.begin()) break;
                    --it;
                    continue;
                }

                // One too many slots of this breadth: Repair by merging.
                // The iterator "it" will point to the first ("leftmost") slot of this breadth
                assert(it != _storage_list.end());
                auto itAfter = std::next(it);
                assert(itAfter != _storage_list.end());
                auto& slotAfter = *itAfter;

                // Update slot breadth
                slot.epochEnd = slotAfter.epochEnd;
                // Merge the two clause databases
                mergeDatabases(slot.cdb, slotAfter.cdb);
                // Delete old slot
                it = _storage_list.erase(itAfter);
                --it; // visit the merged slot again
            }
        }

        void mergeDatabases(AdaptiveClauseStore& into, AdaptiveClauseStore& from) {
            
            // Flush clauses from "from"
            int numExported;
            auto clauses = from.readBuffer();

            // Insert clauses into "into"
            addClausesIntoDatabase(into, clauses, -1, ClauseAdditionMode::MERGE_OR_DROP);
        }

        enum ClauseAdditionMode {INSERT, INSERT_AND_IMPORT, MERGE_OR_DROP};
        std::pair<int, int> addClausesIntoDatabase(AdaptiveClauseStore& cdb, std::vector<int>& clauses, int epoch, ClauseAdditionMode mode) {

            // Track each clause that is being deleted from the database to make room for a new one;
            // also erase each such clause from the filter table
            cdb.setClauseDeletionCallback([&](Mallob::Clause& cls) {
                ProducedClauseCandidate pcc(cls.begin, cls.size, cls.lbd, 0, epoch);
                _filter->erase(pcc);
            });

            // Iterate over all clauses to be added
            BufferReader reader = cdb.getBufferReader(clauses.data(), clauses.size());
            BufferBuilder builder = cdb.getBufferBuilder(nullptr); // for INSERT_AND_IMPORT

            Mallob::Clause cls = reader.getNextIncomingClause();
            int numAccepted = 0;
            int numTotal = 0;
            while (cls.begin != nullptr) {
                ProducedClauseCandidate pcc(cls.begin, cls.size, cls.lbd, 0, epoch);
                if (mode == INSERT || mode == INSERT_AND_IMPORT) {
                    // Attempting to add a new clause from an external source:
                    // check against filter, only insert of not contained yet
                    auto result = _filter->tryRegisterAndInsert(std::move(pcc), &cdb);
                    if (result == GenericClauseFilter::ADMITTED) {
                        numAccepted++;
                        if (mode == INSERT_AND_IMPORT) builder.append(cls);
                    }
                } else if (mode == MERGE_OR_DROP) {
                    // Two databases are being merged: This clause is either merged successfully
                    // or must be erased from the filter.
                    bool accepted = cdb.addClause(cls);
                    if (accepted) {
                        numAccepted++;
                    } else {
                        _filter->erase(pcc);
                    }
                }
                numTotal++;
                cls = reader.getNextIncomingClause();
            }

            // Clear deletion callback again
            cdb.clearClauseDeletionCallbacks();

            if (mode == INSERT_AND_IMPORT) clauses = builder.extractBuffer();

            return std::pair<int, int>(numAccepted, numTotal);
        }

        std::string reportSlots() const {

            std::string out;
            std::string marker = "·";
            for (auto& slot : _storage_list) {
                //LOG(V4_VVER, "SLOT [%i,%i) #lits=%i\n", slot.epochBegin, slot.epochEnd, slot.cdb.getCurrentlyUsedLiterals());
                for (int i = 0; i < slot.epochEnd-slot.epochBegin; i++) {
                    out += marker;
                }
                marker = marker == "-" ? "·" : "-";
            }
            return out;
        }
    };

private:
    BaseSatJob* _job;
    Worker _worker;

    int _last_epoch {0};
    std::vector<std::pair<int, int>> _missing_epoch_intervals;

public:
    HistoricClauseStorage(const AdaptiveClauseStore::Setup& setup, BaseSatJob* job) : 
        _job(job), _worker(setup)  {}

    void importSharing(int epoch, std::vector<int>&& clauses) {
        
        updateMissingIntervals(epoch, epoch+1);

        // fast-forward construction of slots to just before the present clauses
        while (_last_epoch < epoch-1) {
            _last_epoch++;
            LOG(V4_VVER, "HCS insert [%i,%i) empty\n", _last_epoch, _last_epoch+1);
            _worker.addTask(BackgroundTask::INSERT, _last_epoch, _last_epoch+1, std::vector<int>(), false);
        }

        // insert new set of clauses
        LOG(V4_VVER, "HCS insert [%i,%i) buflen=%i\n", epoch, epoch+1, clauses.size());
        _worker.addTask(BackgroundTask::INSERT, epoch, epoch+1, std::move(clauses), false);
        _last_epoch = epoch;

        // request a missing interval yourself (if any are missing)
        printMissingIntervals();
        requestMissingInterval();
    }

    void handleIncomingMissingInterval(int epochBegin, int epochEnd, std::vector<int>&& clauses) {
        updateMissingIntervals(epochBegin, epochEnd);
        LOG(V4_VVER, "HCS insert+import [%i,%i) buflen=%i\n", epochBegin, epochEnd, clauses.size());
        _worker.addTask(BackgroundTask::INSERT_AND_IMPORT, epochBegin, epochEnd, std::move(clauses), false);        
    }

    void handleRequestMissingInterval(bool leftChild, int epochBegin, int epochEnd) {

        for (auto& [missingBegin, missingEnd] : _missing_epoch_intervals) {
            if (areIntervalsOverlapping(missingBegin, missingEnd, epochBegin, epochEnd)) {
                // at least some parts of the interval are missing!
                // ignore the request; it will be re-sent and the missing interval
                // will eventually arrive here.
                return;
            }
        }

        // interval is complete: pick a local storage slot which spans the missing interval
        LOG(V4_VVER, "HCS got request [%i,%i)\n", epochBegin, epochEnd);
        _worker.addTask(BackgroundTask::REQUEST, epochBegin, epochEnd, std::vector<int>(), leftChild);
    }

    void handleFinishedTasks() {

        while (_worker.hasFinishedTask()) {

            auto task = _worker.getFinishedTask();
            //LOG(V4_VVER, "HCS task of type %i done\n", task.type);

            if (task.type == BackgroundTask::REQUEST) {
                LOG(V4_VVER, "HCS [%i,%i) buflen=%i prepared for child\n", 
                    task.epochBegin, task.epochEnd, task.clauses.size());
                JobMessage msg(_job->getId(), _job->getContextId(), _job->getRevision(), 
                    task.epochBegin, MSG_FORWARD_HISTORIC_CLAUSES);
                msg.payload = std::move(task.clauses);
                msg.payload.push_back(task.epochEnd);
                if (task.fromLeftChild && _job->getJobTree().hasLeftChild()) {
                    _job->getJobTree().sendToLeftChild(msg);
                } else if (_job->getJobTree().hasRightChild()) {
                    _job->getJobTree().sendToRightChild(msg);
                }
            }
            if (task.type == BackgroundTask::INSERT_AND_IMPORT) {
                LOG(V4_VVER, "HCS digest historic clauses, buflen=%i\n", task.clauses.size());
                _job->digestHistoricClauses(task.epochBegin, task.epochEnd, task.clauses);
            }
            LOG(V4_VVER, "HCS %i cls / %i lits, layout %s\n",
                task.storageDiagnostics.numClausesInStorage, task.storageDiagnostics.numLitsInStorage, 
                task.storageDiagnostics.slotLayout.c_str());
        }
    }

private:

    void updateMissingIntervals(int epochBegin, int epochEnd) {
    
        // does this close an existing hole?
        for (auto it = _missing_epoch_intervals.begin(); it != _missing_epoch_intervals.end(); ++it) {
            auto& [iBegin, iEnd] = *it;
            if (epochBegin <= iBegin && epochEnd >= iEnd) {
                // full match: delete missing interval
                LOG(V4_VVER, "HCS close full hole\n");
                it = _missing_epoch_intervals.erase(it);
                --it;
            } else if (epochBegin <= iBegin && epochEnd > iBegin && epochEnd < iEnd) {
                // left-end match: reduce missing interval to its right part
                LOG(V4_VVER, "HCS close hole left\n");
                iBegin = epochEnd;
            } else if (epochBegin > iBegin && epochBegin < iEnd && epochEnd >= iEnd) {
                // right-end match: reduce missing interval to its left part
                LOG(V4_VVER, "HCS close hole right\n");
                iEnd = epochBegin;
            } else if (iBegin < epochBegin && epochEnd < iEnd) {
                // match in the middle: separate interval into two smaller ones
                LOG(V4_VVER, "HCS close hole mid\n");
                std::pair<int, int> secondInterval = {epochEnd, iEnd};
                // insert first new interval
                it = _missing_epoch_intervals.insert(it, std::pair<int, int>(iBegin, epochBegin));
                // go to second (existing) interval
                ++it;
                // overwrite with new bounds
                *it = secondInterval;
            }
        }

        // are we creating a new hole between the last epoch and the (first) received epoch?
        if (epochBegin > _last_epoch+1) {
            // add to missing epoch intervals
            if (!_missing_epoch_intervals.empty() && _missing_epoch_intervals.back().second == epochBegin-1) {
                // add to existing interval
                LOG(V4_VVER, "HCS extend hole\n");
                _missing_epoch_intervals.back().second++;
            } else {
                // add new interval
                LOG(V4_VVER, "HCS add hole\n");
                _missing_epoch_intervals.emplace_back(_last_epoch+1, epochBegin);
            }
        }

        printMissingIntervals();
    }

    void printMissingIntervals() {
        std::string missingIntervals;
        for (auto& [begin, end] : _missing_epoch_intervals) 
            missingIntervals += "[" + std::to_string(begin) + "," + std::to_string(end) + ") ";
        if (!missingIntervals.empty())
            LOG(V4_VVER, "HCS missing %s\n", missingIntervals.c_str());
    }

    void requestMissingInterval() {
        if (_missing_epoch_intervals.empty()) return;
        auto [intervBegin, intervEnd] = _missing_epoch_intervals.front();
        LOG(V4_VVER, "HCS requesting [%i,%i)\n", intervBegin, intervEnd);
        JobMessage msg(_job->getId(), _job->getContextId(), _job->getRevision(), 
            _last_epoch, MSG_REQUEST_HISTORIC_CLAUSES);
        msg.payload = {intervBegin, intervEnd};
        _job->getJobTree().sendToParent(msg);
    }
};

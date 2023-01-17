
#pragma once

#include "app/sat/data/clause.hpp"
#include "app/sat/data/produced_clause_candidate.hpp"
#include "util/params.hpp"
#include "base_sat_job.hpp"
#include "../sharing/buffer/adaptive_clause_database.hpp"
#include "../sharing/filter/produced_clause_filter.hpp"
#include "util/logger.hpp"
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/threading.hpp"

#include <limits>

class HistoricClauseStorage {

public:
    struct Resharing {
        int epochBegin {1};
        int epochEnd {-1};
        std::vector<int> clauses;
    };

private:
    AdaptiveClauseDatabase::Setup _setup;
    int _max_same_breadth_slots {3};

    // storage
    struct Slot {
        int epochBegin;
        int epochEnd;
        AdaptiveClauseDatabase cdb;
        Slot(int epochBegin, int epochEnd, AdaptiveClauseDatabase::Setup setup) : 
            epochBegin(epochBegin), epochEnd(epochEnd), cdb(setup) {}
    };
    std::list<Slot> _storage_list;
    ProducedClauseFilter _filter;

    // use of storage for periodic resharing
    Resharing _next_resharing;
    bool _wraparound_resharing {false};

    struct StorageDiagnostics {
        int numLitsInStorage;
        int numClausesInStorage;
        std::string slotLayout;
    };
    struct ResharingTask {
        int epoch;
        std::vector<int> clauses;
        bool finished {false};
        Resharing preparedResharing;
        StorageDiagnostics storageDiagnostics;

        ResharingTask(int epoch, const std::vector<int>& clauses) : epoch(epoch), clauses(clauses) {}
    };
    ConditionVariable _cond_var_resharing_tasks;
    Mutex _mtx_resharing_tasks;
    std::list<ResharingTask> _resharing_tasks;
    BackgroundWorker _bg_worker;

public:
    HistoricClauseStorage(const AdaptiveClauseDatabase::Setup& setup) : 
        _setup(setup), _filter(std::numeric_limits<int>::max(), false) {

        _bg_worker.run([this]() {runBackgroundWorker();});
    }

    void addSharingAndPrepareResharing(int epoch, std::vector<int>& clauses) {
        {
            auto lock = _mtx_resharing_tasks.getLock();
            _resharing_tasks.emplace_back(epoch, clauses);
        }
        _cond_var_resharing_tasks.notify();
    }

    bool hasPreparedResharing() {
        auto lock = _mtx_resharing_tasks.getLock();
        return !_resharing_tasks.empty() && _resharing_tasks.front().finished; 
    }

    Resharing getResharing() {
        auto lock = _mtx_resharing_tasks.getLock();
        ResharingTask task = std::move(_resharing_tasks.front());
        _resharing_tasks.pop_front();
        Resharing resharing = std::move(task.preparedResharing);
        LOG(V4_VVER, "%i cls / %i lits in historic storage\n",
            task.storageDiagnostics.numClausesInStorage, task.storageDiagnostics.numLitsInStorage);
        LOG(V4_VVER, "historic storage layout: %s\n", task.storageDiagnostics.slotLayout.c_str());
        return resharing;
    }

    ~HistoricClauseStorage() {
        {
            auto lock = _mtx_resharing_tasks.getLock();
            _bg_worker.stopWithoutWaiting();
        }
        _cond_var_resharing_tasks.notify();
    }

private:

    void runBackgroundWorker() {

        while (_bg_worker.continueRunning()) {

            _cond_var_resharing_tasks.wait(_mtx_resharing_tasks, 
                [&]() {
                    return !_bg_worker.continueRunning() || 
                        (!_resharing_tasks.empty() && !_resharing_tasks.back().finished);
                }
            );

            _mtx_resharing_tasks.lock();
            if (_resharing_tasks.empty()) continue;
            ResharingTask& task = _resharing_tasks.front();
            _mtx_resharing_tasks.unlock();

            // Create new storage slot of breadth 1
            _storage_list.emplace_back(task.epoch, task.epoch+1, _setup);
            AdaptiveClauseDatabase& cdb = _storage_list.back().cdb;

            // Insert non-duplicate clauses into new storage slot as well as lookup table
            auto [accepted, total] = addClausesIntoDatabase(cdb, task.clauses, task.epoch, ClauseAdditionMode::INSERT_NEW);
            //LOG(V4_VVER, "Added %i/%i cls to HCS\n", accepted, total);

            // Repair invariant of having at most _max_same_breadth_slots slots of the same breadth.
            mergeSlotsAndPrepareResharing();

            int numStoredLits = 0;
            for (auto& slot : _storage_list) {
                numStoredLits += slot.cdb.getCurrentlyUsedLiterals();
            }

            {
                auto lock = _mtx_resharing_tasks.getLock();
                task.preparedResharing.epochBegin = _next_resharing.epochBegin;
                task.preparedResharing.epochEnd = _next_resharing.epochEnd;
                task.preparedResharing.clauses = std::move(_next_resharing.clauses);
                task.storageDiagnostics.numClausesInStorage = _filter.size();
                task.storageDiagnostics.numLitsInStorage = numStoredLits;
                task.storageDiagnostics.slotLayout = reportSlots();
                task.finished = true;
            }

            // Advance resharing range for next time
            if (_wraparound_resharing) _next_resharing.epochEnd = -1;
            _next_resharing.epochBegin = std::max(1, _next_resharing.epochEnd);
            _next_resharing.epochEnd = -1;
            _wraparound_resharing = false;
        }
    }

    void mergeSlotsAndPrepareResharing() {

        if (_storage_list.empty()) return;

        auto it = _storage_list.end(); --it;
        int lastBreadth = 1;
        int numSlotsThisBreadth = 0;
        while (true) {
            auto& slot = *it;

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
                // Slot to be reshared found?
                if (slot.epochBegin == _next_resharing.epochBegin) {
                    _next_resharing.epochEnd = slot.epochEnd;
                    int numExported;
                    _next_resharing.clauses = slot.cdb.exportBufferWithoutDeletion(-1, numExported);
                    // wrap-around the resharing
                    if (slot.epochEnd-slot.epochBegin == 1) _wraparound_resharing = true;
                }
                // finish or go to next
                if (it == _storage_list.begin()) break;
                --it;
                continue;
            }

            // One too many slots of this breadth: Repair by merging.
            // The iterator "it" will point to the first ("leftmost") slot of this breadth
            auto itAfter = std::next(it);
            auto& slotAfter = *itAfter;

            // Update slot breadth
            slot.epochEnd = slotAfter.epochEnd;
            // Merge the two clause databases
            mergeDatabases(slot.cdb, slotAfter.cdb);
            // Delete old slot
            _storage_list.erase(itAfter);

            // Slot to be reshared found (after merging)?
            if (slot.epochBegin == _next_resharing.epochBegin) {
                _next_resharing.epochEnd = slot.epochEnd;
                int numExported;
                _next_resharing.clauses = slot.cdb.exportBufferWithoutDeletion(-1, numExported);
                // wrap-around the resharing
                if (slot.epochEnd-slot.epochBegin == 1) _wraparound_resharing = true;
            }

            // Do NOT advance iterator: Visit the merged slot again
        }
    }

    void mergeDatabases(AdaptiveClauseDatabase& into, AdaptiveClauseDatabase& from) {
        
        // Flush clauses from "from"
        int numExported;
        auto clauses = from.exportBufferWithoutDeletion(-1, numExported);

        // Insert clauses into "into"
        addClausesIntoDatabase(into, clauses, -1, ClauseAdditionMode::MERGE_OR_DROP);
    }

    enum ClauseAdditionMode {INSERT_NEW, MERGE_OR_DROP};
    std::pair<int, int> addClausesIntoDatabase(AdaptiveClauseDatabase& cdb, std::vector<int>& clauses, int epoch, ClauseAdditionMode mode) {

        // Track each clause that is being deleted from the database to make room for a new one;
        // also erase each such clause from the filter table
        cdb.setClauseDeletionCallback([&](Mallob::Clause& cls) {
            ProducedClauseCandidate pcc(cls.begin, cls.size, cls.lbd, 0, epoch);
            _filter.erase(pcc);
        });

        // Iterate over all clauses to be added
        BufferReader reader = cdb.getBufferReader(clauses.data(), clauses.size());
        Mallob::Clause cls = reader.getNextIncomingClause();
        int numAccepted = 0;
        int numTotal = 0;
        while (cls.begin != nullptr) {
            ProducedClauseCandidate pcc(cls.begin, cls.size, cls.lbd, 0, epoch);
            if (mode == INSERT_NEW) {
                // Attempting to add a new clause from an external source:
                // check against filter, only insert of not contained yet
                auto result = _filter.tryRegisterAndInsert(std::move(pcc), cdb);
                if (result == ProducedClauseFilter::ADMITTED) numAccepted++;
            } else if (mode == MERGE_OR_DROP) {
                // Two databases are being merged: This clause is either merged successfully
                // or must be erased from the filter.
                bool accepted = cdb.addClause(cls);
                if (accepted) {
                    numAccepted++;
                } else {
                    _filter.erase(pcc);
                }
            }
            numTotal++;
            cls = reader.getNextIncomingClause();
        }

        // Clear deletion callback again
        cdb.clearClauseDeletionCallback();

        return std::pair<int, int>(numAccepted, numTotal);
    }

    std::string reportSlots() const {

        std::string out;
        std::string marker = "·";
        for (auto& slot : _storage_list) {
            //LOG(V2_INFO, "SLOT [%i,%i) #lits=%i\n", slot.epochBegin, slot.epochEnd, slot.cdb.getCurrentlyUsedLiterals());
            for (int i = 0; i < slot.epochEnd-slot.epochBegin; i++) {
                if (slot.epochBegin+i >= _next_resharing.epochBegin && slot.epochBegin+i < _next_resharing.epochEnd) {
                    out += "O";
                } else {
                    out += marker;
                }
            }
            marker = marker == "-" ? "·" : "-";
        }
        return out;
    }
};

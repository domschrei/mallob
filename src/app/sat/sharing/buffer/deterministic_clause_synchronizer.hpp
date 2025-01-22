
#pragma once

#include <functional>
#include <deque>

#include "../../data/clause.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "util/logger.hpp"
#include "util/sys/threading.hpp"

class DeterministicClauseSynchronizer {

public:
    struct ClauseInsertionCall {
        int solverId;
        int solverRevision;
        Mallob::Clause clause;
        std::vector<int> condLits;
    };
    typedef std::function<void(const ClauseInsertionCall&)> CbAdmitClause;

private:
    CbAdmitClause _cb_admit_clause;
    std::vector<std::shared_ptr<PortfolioSolverInterface>>& _solvers;
    
    Mutex _mtx_flush;
    int _num_nonempty_queues {0};
    struct AdmissionQueue {
        std::deque<ClauseInsertionCall> queue;
        int numInserted {0};
        bool waiting {false};
    };
    std::vector<AdmissionQueue> _admission_queues;

    int _nb_insertions_until_sync {10'000};
    int _nb_waiting_for_sync {0};
    Mutex _mtx_sync;
    ConditionVariable _cond_var_sync;

    int _min_solver_id_with_result {-1};

public:
    DeterministicClauseSynchronizer(std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
            size_t numOrigClauses, CbAdmitClause cb) :
        _cb_admit_clause(cb), _solvers(solvers), _admission_queues(_solvers.size()),
        _nb_insertions_until_sync(std::floor(0.1*approximateConflictsPerSecond(numOrigClauses))) {}
    ~DeterministicClauseSynchronizer() {
        if (isWaitingForSync()) syncAndCheckForLocalWinner(-1);
    }

    void insertBlocking(int solverId, int solverRevision, const Mallob::Clause& clause, const std::vector<int>& condLits) {
        
        waitForSync(solverId);

        {
            auto lock = _mtx_flush.getLock();

            auto& q = _admission_queues.at(solverId);
            bool emptyBefore = q.queue.empty();
            q.queue.push_back(ClauseInsertionCall {solverId, solverRevision, clause.copy(), condLits});
            q.numInserted++;

            if (emptyBefore) {
                _num_nonempty_queues++;
                while (_num_nonempty_queues == _admission_queues.size()) {
                    // flush one clause from each queue
                    int numNonEmptyQueuesAfterFlush = 0;
                    for (auto& otherQ : _admission_queues) {
                        assert(!otherQ.queue.empty());
                        auto call = std::move(otherQ.queue.front());
                        otherQ.queue.pop_front();
                        if (!otherQ.queue.empty()) numNonEmptyQueuesAfterFlush++;
                        _cb_admit_clause(call);
                    }
                    _num_nonempty_queues = numNonEmptyQueuesAfterFlush;
                }
            }

            if (q.numInserted % _nb_insertions_until_sync == 0) {
                // Sync! Sleep until clause exchange has been done.
                {
                    auto lock = _mtx_sync.getLock();
                    assert(!q.waiting);
                    q.waiting = true;
                    _nb_waiting_for_sync++;
                }
                _cond_var_sync.notify();
            }
        }

        waitForSync(solverId);
    }

    void notifySolverDone(int localId) {
        {
            auto lock = _mtx_sync.getLock();
            auto& q = _admission_queues[localId];
            if (!q.waiting) {
                q.waiting = true;
                _nb_waiting_for_sync++;
                int globalId = _solvers[localId]->getGlobalId();
                if (_min_solver_id_with_result == -1 || _min_solver_id_with_result > globalId)
                    _min_solver_id_with_result = globalId;
            }
        }
        _cond_var_sync.notify();
    }

    bool areAllSolversSyncReady() {
        auto lock = _mtx_sync.getLock();
        return _nb_waiting_for_sync == _admission_queues.size();
    }

    int waitUntilSyncReadyAndReturnSolverIdWithResult() {
        // Wait until all solvers are waiting for sync
        _cond_var_sync.wait(_mtx_sync, [&]() {return _nb_waiting_for_sync == _admission_queues.size();});
        return _min_solver_id_with_result;
    }

    bool isWaitingForSync() {
        auto lock = _mtx_sync.getLock();
        return _nb_waiting_for_sync == _admission_queues.size();
    }

    bool syncAndCheckForLocalWinner(int globalWinningId) {
        bool hasWinningSolver = false;
        {
            auto lock = _mtx_sync.getLock();
            assert(_nb_waiting_for_sync == _admission_queues.size());
            for (int i = 0; i < _admission_queues.size(); ++i) {
                auto& q = _admission_queues[i];
                assert(q.waiting);
                if (_solvers[i]->getGlobalId() == globalWinningId) {
                    hasWinningSolver = true;
                }
                q.waiting = false;
            }
            _nb_waiting_for_sync = 0;
        }
        _cond_var_sync.notify();
        return hasWinningSolver;
    }

private:
    double approximateConflictsPerSecond(size_t numOrigClauses) {

        // limits
        if (numOrigClauses < 1000) return 100'000;
        if (numOrigClauses > 5e7) return 50;

        // function maps milliclauses to kiloseconds per conflict
        const double x = 0.001 * numOrigClauses;

        const double a = 6.76629106e-14;
        const double b = -1.44864905e-08;
        const double c = 9.68449085e-04;
        const double kSecsPerConflict = a * std::pow(x, 3) + b * std::pow(x, 2) + c * x + 0.01;

        const double secsPerConflict = std::min(0.02, 0.001 * kSecsPerConflict);
        const double conflictsPerSec = 1 / secsPerConflict;
        LOG(V2_INFO, "Det. solving: approximated %ld clauses to result in %.3f conflicts per second\n", 
            numOrigClauses, conflictsPerSec);

        assert(conflictsPerSec >= 50);
        assert(conflictsPerSec <= 100'000);
        return conflictsPerSec;
    }

private:
    void waitForSync(int solverId) {
        bool& waiting = _admission_queues.at(solverId).waiting;
        if (!waiting) return;
        LOG(V5_DEBG, "%i : WAIT_FOR_SYNC\n", solverId);
        _cond_var_sync.wait(_mtx_sync, [&]() {return !waiting;});
        LOG(V5_DEBG, "%i : END_WAIT_FOR_SYNC\n", solverId);
    }
};

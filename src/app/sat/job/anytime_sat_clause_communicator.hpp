
#pragma once

#include <future>

#include "app/sat/data/clause.hpp"
#include "util/params.hpp"
#include "util/hashing.hpp"
#include "../sharing/buffer/adaptive_clause_database.hpp"
#include "data/job_transfer.hpp"
#include "app/job.hpp"
#include "base_sat_job.hpp"
#include "clause_history.hpp"
#include "comm/job_tree_all_reduction.hpp"
#include "clause_sharing_session.hpp"

class AnytimeSatClauseCommunicator {

private:
    const Parameters _params;
    BaseSatJob* _job = NULL;
    bool _suspended = false;

    const int _clause_buf_base_size;
    const float _clause_buf_discount_factor;
    const bool _use_cls_history;

    AdaptiveClauseDatabase _cdb;
    ClauseHistory _cls_history;

    std::list<ClauseSharingSession> _sessions;

    int _current_epoch = 0;
    float _time_of_last_epoch_initiation = 0;

public:
    AnytimeSatClauseCommunicator(const Parameters& params, BaseSatJob* job) : _params(params), _job(job), 
        _clause_buf_base_size(_params.clauseBufferBaseSize()), 
        _clause_buf_discount_factor(_params.clauseBufferDiscountFactor()),
        _use_cls_history(params.collectClauseHistory()),
        _cdb([&]() {
            AdaptiveClauseDatabase::Setup setup;
            setup.maxClauseLength = _params.strictClauseLengthLimit();
            setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
            setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
            setup.numLiterals = 0;
            return setup;
        }()),
        _cls_history(_params, _job->getBufferLimit(_job->getJobTree().getCommSize(), MyMpi::ALL), *job, _cdb) {

        _time_of_last_epoch_initiation = Timer::elapsedSecondsCached();
    }

    ~AnytimeSatClauseCommunicator() {
        _sessions.clear();
    }

    void communicate();
    void handle(int source, int mpiTag, JobMessage& msg);

    void feedHistoryIntoSolver();
    bool isDestructible();

private:
    bool tryInitiateSharing();
    inline ClauseSharingSession& currentSession() {return _sessions.back();}
};

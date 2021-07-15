
#ifndef DOMPASCH_MALLOB_ANYTIME_SAT_CLAUSE_COMMUNICATOR_H
#define DOMPASCH_MALLOB_ANYTIME_SAT_CLAUSE_COMMUNICATOR_H

#include "util/params.hpp"
#include "util/hashing.hpp"
#include "hordesat/sharing/lockfree_clause_database.hpp"
#include "data/job_transfer.hpp"
#include "app/job.hpp"
#include "base_sat_job.hpp"
#include "clause_history.hpp"

const int MSG_GATHER_CLAUSES = 417;
const int MSG_DISTRIBUTE_CLAUSES = 418;

class AnytimeSatClauseCommunicator {

private:
    Parameters _params;
    BaseSatJob* _job = NULL;

    const int _clause_buf_base_size;
    const float _clause_buf_discount_factor;
    const bool _use_checksums;
    const bool _use_cls_history;

    ClauseHistory _cls_history;
    LockfreeClauseDatabase _cdb;
    std::vector<std::vector<int>> _clause_buffers;
    int _num_aggregated_nodes;
    int _current_epoch = 0;

    bool _initialized = false;

public:
    AnytimeSatClauseCommunicator(const Parameters& params, BaseSatJob* job) : _params(params), _job(job), 
        _clause_buf_base_size(_params.clauseBufferBaseSize()), 
        _clause_buf_discount_factor(_params.clauseBufferDiscountFactor()),
        _use_checksums(params.useChecksums()),
        _use_cls_history(params.collectClauseHistory()),
        _cls_history(_params, getBufferLimit(_job->getJobTree().getCommSize(), MyMpi::ALL), *job),
        _cdb(_params.hardMaxClauseLength(), _params.maxLbdPartitioningSize(), _clause_buf_base_size, 1),
        _num_aggregated_nodes(0) {

        _initialized = true;
    }
    bool canSendClauses();
    void sendClausesToParent();
    void handle(int source, JobMessage& msg);
    void suspend();

private:
    
    size_t getBufferLimit(int numAggregatedNodes, MyMpi::BufferQueryMode mode);

    std::vector<int> prepareClauses();
    void broadcastAndLearn(std::vector<int>& clauses);
    void learnClauses(std::vector<int>& clauses);
    void sendClausesToChildren(std::vector<int>& clauses);

    std::vector<int> merge(size_t maxSize);
    bool testConsistency(std::vector<int>& buffer, size_t maxSize, bool sortByLbd);

    std::vector<int> getEmptyBuffer() {
        return std::vector<int>();
    }
};

#endif
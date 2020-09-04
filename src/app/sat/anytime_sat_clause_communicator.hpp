
#ifndef DOMPASCH_MALLOB_ANYTIME_SAT_CLAUSE_COMMUNICATOR_H
#define DOMPASCH_MALLOB_ANYTIME_SAT_CLAUSE_COMMUNICATOR_H

#include "util/params.hpp"
#include "data/job_transfer.hpp"
#include "app/job.hpp"
#include "base_sat_job.hpp"
#include "hordesat/utilities/clause_filter.hpp"

const int MSG_GATHER_CLAUSES = 417;
const int MSG_DISTRIBUTE_CLAUSES = 418;

class AnytimeSatClauseCommunicator {

private:
    const Parameters& _params;
    BaseSatJob* _job = NULL;

    const int _clause_buf_base_size;
    const float _clause_buf_discount_factor;

    std::vector<std::vector<int>> _clause_buffers;
    std::unordered_set<std::vector<int>, ClauseFilter::ClauseHasher> _clause_filter;
    int _num_aggregated_nodes;

    bool _initialized = false;

public:
    AnytimeSatClauseCommunicator(const Parameters& params, BaseSatJob* job) : _params(params), _job(job), 
        _clause_buf_base_size(_params.getIntParam("cbbs")), 
        _clause_buf_discount_factor(_params.getFloatParam("cbdf")),
        _num_aggregated_nodes(0) {

        _initialized = true;
    }
    bool canSendClauses();
    void sendClausesToParent();
    void handle(int source, JobMessage& msg);

private:
    
    float getBufferLimit(int numAggregatedNodes);

    std::vector<int> prepareClauses(); 
    void learnClauses(const std::vector<int>& clauses);
    void sendClausesToChildren(const std::vector<int>& clauses);

    std::vector<int> merge(const std::vector<std::vector<int>*>& buffers, size_t maxSize);
    bool testConsistency(const std::vector<int>& buffer);
};

#endif
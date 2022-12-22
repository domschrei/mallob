
#pragma once

#include "comm/mympi.hpp"
#include "util/assert.hpp"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/tsl/robin_map.h"
#include "util/hashing.hpp"

class Job; // fwd declaration

struct JobIdContextIdHasher {
    size_t operator()(const std::pair<int, ctx_id_t>& obj) const {
        size_t val = 314'159UL;
        hash_combine(val, obj.first);
        hash_combine(val, obj.second);
        return val;
    }
};
typedef tsl::robin_map<std::pair<int, ctx_id_t>, Job*, JobIdContextIdHasher> AppMessageTable;

class AppMessageSubscription {

private:
    AppMessageTable& _table;
    int _job_id;
    ctx_id_t _ctx_id;

    static ctx_id_t _running_ctx_id;

public:
    AppMessageSubscription(AppMessageTable& table, int jobId, Job* job) : 
            _table(table), _job_id(jobId) {

        // ID must be unique among all MPI processes
        // and also unique within this process
        _ctx_id = _running_ctx_id * MyMpi::size(MPI_COMM_WORLD) + MyMpi::rank(MPI_COMM_WORLD);
        _running_ctx_id++;

        registerJobInTable(job);
    }

    ctx_id_t getContextId() const {
        return _ctx_id;
    }

    void destroy() {
        unregisterJobFromTable();
    }

    ~AppMessageSubscription() {
        unregisterJobFromTable();
    }

private:
    void registerJobInTable(Job* job) {
        std::pair<int, ctx_id_t> pair(_job_id, _ctx_id);
        _table[pair] = job;
    }

    void unregisterJobFromTable() {
        std::pair<int, ctx_id_t> pair(_job_id, _ctx_id);
        _table.erase(pair);
    }
};

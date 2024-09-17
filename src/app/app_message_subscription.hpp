
#pragma once

#include <stddef.h>
#include <utility>

#include "comm/mympi.hpp"
#include "util/assert.hpp"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/tsl/robin_map.h"
#include "util/hashing.hpp"
#include "comm/mpi_base.hpp"
#include "robin_hash.h"

class Job; // fwd declaration

class AppMessageListener {
public:
    virtual int getId() const = 0;
    virtual void communicate(int source, int mpiTag, JobMessage& msg) = 0;
};

typedef tsl::robin_map<ctx_id_t, AppMessageListener*> AppMessageTable;

class AppMessageSubscription {

private:
    AppMessageTable& _table;
    int _id;
    ctx_id_t _ctx_id;

    static ctx_id_t _running_ctx_id;

public:
    AppMessageSubscription(AppMessageTable& table, AppMessageListener* listener) :
            _table(table), _id(listener->getId()) {

        // ID must be unique among all MPI processes
        // and also unique within this process
        _ctx_id = _running_ctx_id * MyMpi::size(MPI_COMM_WORLD) + MyMpi::rank(MPI_COMM_WORLD);
        _running_ctx_id++;

        registerListenerInTable(listener);
    }

    ctx_id_t getContextId() const {
        return _ctx_id;
    }

    void destroy() {
        unregisterListenerFromTable();
    }

    ~AppMessageSubscription() {
        unregisterListenerFromTable();
    }

private:
    void registerListenerInTable(AppMessageListener* l) {
        _table[_ctx_id] = l;
    }

    void unregisterListenerFromTable() {
        _table.erase(_ctx_id);
    }
};

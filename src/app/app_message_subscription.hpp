
#pragma once

#include <stddef.h>
#include <utility>

#include "comm/mympi.hpp"
#include "util/assert.hpp"
#include "data/job_transfer.hpp"
#include "util/logger.hpp"
#include "util/sys/threading.hpp"
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

class AppMessageTable {
    Mutex mtx;
    tsl::robin_map<ctx_id_t, AppMessageListener*> map;
public:
    void registerListenerInTable(ctx_id_t ctxId, AppMessageListener* l) {
        auto lock = mtx.getLock();
        map[ctxId] = l;
    }
    void unregisterListenerFromTable(ctx_id_t ctxId) {
        auto lock = mtx.getLock();
        map.erase(ctxId);
    }
    // RAII-style-locked handle for a particular listener. Valid if listener != nullptr.
    struct ListenerHandle {
        std::unique_lock<std::mutex> lock;
        AppMessageListener* listener {nullptr};
    };
    ListenerHandle accessListener(ctx_id_t ctxId) {
        auto lock = mtx.getLock();
        auto it = map.find(ctxId);
        if (it == map.end()) return {};
        return {std::move(lock), it.value()};
    };
};

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

        _table.registerListenerInTable(_ctx_id, listener);
    }

    ctx_id_t getContextId() const {
        return _ctx_id;
    }

    void destroy() {
        _table.unregisterListenerFromTable(_ctx_id);
    }

    ~AppMessageSubscription() {
        destroy();
    }
};

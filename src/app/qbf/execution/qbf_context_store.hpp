
#pragma once

#include "util/logger.hpp"
#include "util/tsl/robin_map.h"

#include "qbf_context.hpp"

class QbfContextStore {

private:
    static Mutex _mtx_map;
    static tsl::robin_map<int, std::pair<std::unique_ptr<Mutex>, std::unique_ptr<QbfContext>>> _map;

public:
    class ExclusiveWrapper {
    private:
        Mutex* _mtx {nullptr};
        QbfContext* _ctx {nullptr};
    public:
        ExclusiveWrapper() {}
        ExclusiveWrapper(Mutex& mtx, QbfContext& ctx) : _mtx(&mtx), _ctx(&ctx) {
            _mtx->lock();
        }
        ExclusiveWrapper(ExclusiveWrapper&& other) : _mtx(other._mtx), _ctx(other._ctx) {
            other._mtx = nullptr;
            other._ctx = nullptr;
        }
        ExclusiveWrapper(const ExclusiveWrapper& other) = delete;
        ExclusiveWrapper& operator=(ExclusiveWrapper&& other) {
            _mtx = other._mtx;
            _ctx = other._ctx;
            other._mtx = nullptr;
            other._ctx = nullptr;
            return *this;
        }
        ExclusiveWrapper& operator=(const ExclusiveWrapper& other) = delete;
        ~ExclusiveWrapper() {
            if (_mtx != nullptr) _mtx->unlock();
        }
        operator bool() {
            return _mtx != nullptr;
        }
        QbfContext& operator*() {
            return *_ctx;
        }
        QbfContext* operator->() {
            return _ctx;
        }
    };

public:
    static void create(int id, QbfContext&& baseCtx) {
        LOG(V3_VERB, "QBF CREATE_CTX %i\n", id);
        auto lock = _mtx_map.getLock();
        _map[id] = {std::unique_ptr<Mutex>(new Mutex()), std::unique_ptr<QbfContext>(new QbfContext(baseCtx))};
    }

    // Try to acquire the QbfContext of the specified ID.
    // If this context does not exist, returns an empty object
    // (evaluate it as a bool to check).
    // On success, the context can be accessed via operator->
    // as well as operator*.
    static ExclusiveWrapper tryAcquire(int id) {
        auto lock = _mtx_map.getLock();
        if (!_map.count(id)) return {};
        return ExclusiveWrapper(*_map[id].first, *_map[id].second);
    }

    static void erase(int id) {
        LOG(V3_VERB, "QBF ERASE_CTX %i\n", id);
        auto lock = _mtx_map.getLock();
        // just "touch" the lock once to make sure no one owns it any more
        _map[id].first->lock();
        _map[id].first->unlock();
        _map.erase(id);
    }
};

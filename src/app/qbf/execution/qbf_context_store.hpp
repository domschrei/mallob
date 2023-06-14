
#pragma once

#include "util/tsl/robin_map.h"

#include "qbf_context.hpp"

class QbfContextStore {

private:
    static Mutex _mtx_map;
    static tsl::robin_map<int, std::unique_ptr<QbfContext>> _map;

public:
    static QbfContext& create(int id, QbfContext&& baseCtx) {
        auto lock = _mtx_map.getLock();
        _map[id].reset(new QbfContext(baseCtx));
        return *_map[id];
    }

    static bool has(int id) {
        auto lock = _mtx_map.getLock();
        return _map.count(id);
    }

    static QbfContext& get(int id) {
        auto lock = _mtx_map.getLock();
        return *_map[id];
    }

    static void erase(int id) {
        auto lock = _mtx_map.getLock();
        _map.erase(id);
    }
};

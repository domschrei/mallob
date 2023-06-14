
#pragma once

#include "comm/msg_queue/message_subscription.hpp"
#include "data/app_configuration.hpp"
#include "util/sys/threading.hpp"
#include "util/tsl/robin_map.h"
#include <list>

class PermanentCache {

private:
    Mutex _mtx_map;
    tsl::robin_map<int, std::string> _data_map;
    tsl::robin_map<int, std::list<MessageSubscription>> _msg_map;
    static PermanentCache _perm_cache_instance;

public:
    static PermanentCache& getMainInstance() {
        return _perm_cache_instance;
    }

    void putData(int key, const std::string& val) {
        auto lock = _mtx_map.getLock();
        _data_map[key] = val;
    }
    bool hasData(int key) {
        auto lock = _mtx_map.getLock();
        return _data_map.count(key);
    }
    std::string getData(int key) {
        auto lock = _mtx_map.getLock();
        if (!_data_map.count(key)) return "";
        return _data_map.at(key);
    }

    void putMsgSubscription(int key, MessageSubscription&& sub) {
        auto lock = _mtx_map.getLock();
        _msg_map[key].push_back(std::move(sub));
    }

    void erase(int key) {
        auto lock = _mtx_map.getLock();
        _data_map.erase(key);
    }
};

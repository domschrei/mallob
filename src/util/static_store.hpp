
#pragma once

#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include "util/assert.hpp"

#include <string>
#include <map>

template <typename T>
class StaticStore {

private:
    static Mutex _mtx_map;
    static std::map<std::string, T> _map;

public:
    static void insert(const std::string& key, T&& val) {
        auto lock = _mtx_map.getLock();
        _map.insert({key, std::move(val)});
    }
    static void insert(const std::string& key, const T& val) {
        auto lock = _mtx_map.getLock();
        _map.insert({key, val});
    }
    static T extract(const std::string& key) {
        auto lock = _mtx_map.getLock();
        auto it = _map.find(key);
        assert(it != _map.end() || log_return_false( "[ERROR] StaticStore: key \"%s\" not found!\n", key.c_str()));
        T val = std::move(it->second);
        _map.erase(key);
        return val;
    }
};

template <typename T>
Mutex StaticStore<T>::_mtx_map;
template <typename T>
std::map<std::string, T> StaticStore<T>::_map;


#pragma once

#include "util/sys/threading.hpp"
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
        T val = std::move(_map[key]);
        _map.erase(key);
        return val;
    }
};

template <typename T>
Mutex StaticStore<T>::_mtx_map;
template <typename T>
std::map<std::string, T> StaticStore<T>::_map;

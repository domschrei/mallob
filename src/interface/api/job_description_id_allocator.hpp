
#pragma once

#include "robin_map.h"
#include "util/sys/threading.hpp"
#include <string>

class JobDescriptionIdAllocator {

private:
    int _client_rank;
    int _nb_clients;
    int _running_id {1};

    tsl::robin_map<std::string, int> _map;
    Mutex _mtx_map;

public:
    JobDescriptionIdAllocator(int clientRank, int numClients) : _client_rank(clientRank), _nb_clients(numClients) {}
    
    bool hasId(const std::string& label) {
        auto lock = _mtx_map.getLock();
        return _map.contains(label);
    }
    int getId(const std::string& label) {
        auto lock = _mtx_map.getLock();
        if (!_map.contains(label)) {
            _map[label] = nextId();
        }
        return _map[label];
    }

private:
    int nextId() {
        int nextId = _running_id;
        _running_id += _nb_clients;
        return nextId;
    }
};

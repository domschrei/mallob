
#pragma once

#include "interface/api/api_connector.hpp"
#include <memory>

class APIRegistry {

private:
    static std::shared_ptr<APIConnector> _connector;

public:
    static void put(std::shared_ptr<APIConnector>& connector) {
        _connector = connector;
    }
    static APIConnector* get() {
        return _connector.get();
    }
};

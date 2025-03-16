
#pragma once

#include "interface/api/api_connector.hpp"
#include <memory>

class APIRegistry {

private:
    static std::unique_ptr<APIConnector> _connector;

public:
    static void put(APIConnector* connector) {
        _connector.reset(connector);
    }
    static APIConnector* get() {
        return _connector.get();
    }
};

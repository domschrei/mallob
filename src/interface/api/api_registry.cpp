
#include "api_registry.hpp"
#include "interface/api/api_connector.hpp"

std::unique_ptr<APIConnector> APIRegistry::_connector;

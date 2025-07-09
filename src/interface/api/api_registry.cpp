
#include "api_registry.hpp"
#include "interface/api/api_connector.hpp"

std::shared_ptr<APIConnector> APIRegistry::_connector;
APIRegistry::ExternalAPIConnector APIRegistry::_ext_connector;


#pragma once

#include <future>

#include "interface/connector.hpp"
#include "interface/json_interface.hpp"

class APIConnector : public Connector {

public:
    static std::function<void(nlohmann::json&)> CALLBACK_IGNORE;

private:
    JsonInterface& _interface;
    const Parameters& _params;
    Logger _logger;

public:
    APIConnector(JsonInterface& interface, const Parameters& params, Logger&& logger) 
        : _interface(interface), _params(params), _logger(std::move(logger)) {}
    ~APIConnector() {}

    /*
    Submits any kind of JSON request, immediately returns the status of processing the JSON,
    and _may_ later call `callback(nlohmann::json& response)` to return a response JSON.
    */
    JsonInterface::Result submit(nlohmann::json& data, std::function<void(nlohmann::json&)> callback = CALLBACK_IGNORE);
    
    /*
    Submits a kind of JSON request which requires a JSON response. Processes the request,
    waits for its completion and returns the response. If the request could not be processed properly,
    an empty JSON is returned.
    */
    nlohmann::json processBlocking(nlohmann::json& input);

    /*
    Submits a kind of JSON request which requires a JSON response. Processes the request,
    returns immediately and puts the response in the returned promise's future.
    If the request could not be processed properly, an empty JSON is returned.
    */
    std::promise<nlohmann::json> processAsync(nlohmann::json& input);
};

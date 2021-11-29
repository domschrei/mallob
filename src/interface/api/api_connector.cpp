
#include "api_connector.hpp"

std::function<void(nlohmann::json&)> APIConnector::CALLBACK_IGNORE = [](nlohmann::json&) {};

nlohmann::json APIConnector::processBlocking(const nlohmann::json& input) {
    auto promise = processAsync(input);
    return promise.get_future().get();
}

std::promise<nlohmann::json> APIConnector::processAsync(const nlohmann::json& input) {
    std::promise<nlohmann::json> promise;
    auto result = _interface.handle(input, [&](nlohmann::json& response) {
        promise.set_value(response);
    });
    if (result == JsonInterface::Result::DISCARD) {
        promise.set_value(nlohmann::json());
    }
    return promise;
}

JsonInterface::Result APIConnector::submit(const nlohmann::json& data, std::function<void(nlohmann::json&)> callback) {
    return _interface.handle(data, callback);
}
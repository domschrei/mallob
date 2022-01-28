
#include "api_connector.hpp"

std::function<void(nlohmann::json&)> APIConnector::CALLBACK_IGNORE = [](nlohmann::json&) {};

nlohmann::json APIConnector::processBlocking(nlohmann::json& input) {
    auto promise = processAsync(input);
    return promise.get_future().get();
}

std::promise<nlohmann::json> APIConnector::processAsync(nlohmann::json& input) {
    std::promise<nlohmann::json> promise;
    auto result = _interface.handle(input, [&](nlohmann::json& response) {
        promise.set_value(std::move(response));
    });
    if (result == JsonInterface::Result::DISCARD) {
        promise.set_value(nlohmann::json());
    }
    return promise;
}

JsonInterface::Result APIConnector::submit(nlohmann::json& data, std::function<void(nlohmann::json&)> callback) {
    return _interface.handle(data, callback);
}
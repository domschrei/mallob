
#pragma once

#include "comm/msg_queue/message_subscription.hpp"
#include "comm/msgtags.h"
#include "interface/api/api_connector.hpp"
#include "interface/json_interface.hpp"
#include "robin_map.h"
#include "util/logger.hpp"
#include "util/sys/threading.hpp"
#include <memory>
#include <string>

class APIRegistry {

private:
    static std::shared_ptr<APIConnector> _connector;

    struct ExternalAPIConnector {
        Mutex mtx;
        std::unique_ptr<MessageSubscription> sub;
        tsl::robin_map<std::string, std::function<void(JsonInterface::Result, nlohmann::json&)>> jobUserNameToCallback;

        void submit(int recvRank, nlohmann::json& json, std::function<void(JsonInterface::Result, nlohmann::json&)> callback) {

            auto jobIdentifier = json["user"].get<std::string>() + "." + json["name"].get<std::string>();
            auto lock = mtx.getLock();
            jobUserNameToCallback[jobIdentifier] = callback;

            auto packed = nlohmann::json::to_msgpack(json);
            MyMpi::isend(recvRank, MSG_SUBMIT_JOB_TO_CLIENT, std::move(packed), false);

            if (!sub) sub.reset(new MessageSubscription(MSG_RESPOND_TO_JOB_SUBMISSION, [this](MessageHandle& h) {
                LOG(V2_INFO, "MAXSAT RECEIVED JOB RESPONSE\n");
                auto packed = h.moveRecvData();
                JsonInterface::Result res = (JsonInterface::Result) packed.back(); packed.pop_back();
                nlohmann::json response = nlohmann::json::from_msgpack(packed.begin(), packed.end());

                auto jobIdentifier = response["user"].get<std::string>() + "." + response["name"].get<std::string>();
                std::function<void(JsonInterface::Result, nlohmann::json&)> callback;
                {
                    auto lock = mtx.getLock();
                    callback = jobUserNameToCallback[jobIdentifier];
                }
                callback(res, response);
            }));
        }
    };
    static ExternalAPIConnector _ext_connector;

public:
    static void put(APIConnector* connector) {
        _connector.reset(connector);
    }
    static APIConnector& get() {
        return *_connector.get();
    }
    static void sendJobSubmissionToRank(int recvRank, nlohmann::json& json, std::function<void (JsonInterface::Result, nlohmann::json &)> callback) {
        _ext_connector.submit(recvRank, json, callback);
    }
    static void close() {
        _connector.reset();
    }
};

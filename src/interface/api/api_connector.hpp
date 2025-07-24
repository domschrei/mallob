
#pragma once

#include <future>
#include <functional>
#include <string>
#include <utility>

#include "comm/msg_queue/message_handle.hpp"
#include "comm/msg_queue/message_subscription.hpp"
#include "comm/msgtags.h"
#include "interface/connector.hpp"
#include "interface/json_interface.hpp"
#include "robin_map.h"
#include "util/json.hpp"
#include "util/logger.hpp"
#include "util/sys/threading.hpp"

class Parameters;

class APIConnector : public Connector {

public:
    static std::function<void(nlohmann::json&)> CALLBACK_IGNORE;

private:
    JsonInterface& _interface;
    const Parameters& _params;
    Logger _logger;
    bool _active {true};

    MessageSubscription _subscription_job_submission;
    Mutex _mtx_job_user_name;
    tsl::robin_map<std::string, JsonInterface::Result> _job_user_name_to_submit_result;

public:
    APIConnector(JsonInterface& interface, const Parameters& params, Logger&& logger) 
        : _interface(interface), _params(params), _logger(std::move(logger)),
        _subscription_job_submission(MSG_SUBMIT_JOB_TO_CLIENT, [&](MessageHandle& h) {
            handleExternalJobSubmission(h);
        }) {}
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

    bool active() const {return _interface.isActive();}

private:
    void handleExternalJobSubmission(MessageHandle& h) {

        auto data = h.moveRecvData();
        nlohmann::json json = nlohmann::json::from_msgpack(data.data(), data.data()+data.size());
        auto jobIdentifier = json["user"].get<std::string>() + "." + json["name"].get<std::string>();

        auto result = submit(json, [this, sendRank = h.source](nlohmann::json& response) {
            auto jobIdentifier = response["user"].get<std::string>() + "." + response["name"].get<std::string>();

            // Make sure that the result of the submit operation has been stored
            JsonInterface::Result res;
            while (true) {
                _mtx_job_user_name.lock();
                if (!_job_user_name_to_submit_result.count(jobIdentifier)) {
                    _mtx_job_user_name.unlock();
                    usleep(1000);
                    continue;
                }
                res = _job_user_name_to_submit_result[jobIdentifier];
                _mtx_job_user_name.unlock();
                break;
            }

            // Send response with the parcelled JSON response and the result returned from the submit call
            auto packed = nlohmann::json::to_msgpack(response);
            packed.push_back(res);
            MyMpi::isend(sendRank, MSG_RESPOND_TO_JOB_SUBMISSION, std::move(packed));
        });

        if (result == JsonInterface::DISCARD) {
            // Immediately send response with the DISCARD result returned from the submit call
            std::vector<uint8_t> packed(1, result);
            MyMpi::isend(h.source, MSG_RESPOND_TO_JOB_SUBMISSION, std::move(packed));
            return;
        }

        // Store result of the submit operation so that the callback can later make use of it
        auto lock = _mtx_job_user_name.getLock();
        _job_user_name_to_submit_result[jobIdentifier] = result;
    }
};

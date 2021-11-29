
#pragma once

#include "util/sys/proc.hpp"
#include "interface/connector.hpp"
#include "interface/json_interface.hpp"
#include "socket.hpp"
#include "util/params.hpp"

class SocketConnector : public Connector {

private:
    Socket* _socket;

public:
    SocketConnector(Parameters& params, JsonInterface& interface) {

        // Settings for the local socket
        Socket::SocketSettings settings;
        settings.address = "/tmp/mallob_" + std::to_string(Proc::getPid()) + ".sk";
        settings.maxNumConnections = params.activeJobsPerClient();
        settings.maxMsgSize = 65536;

        // Callback to receive a JSON request from a certain user connection
        settings.receiveCallback = [&interface](Connection& conn, nlohmann::json& json) {
            // Callback to return an answer for the request being submitted
            auto cb = [&conn](nlohmann::json& result) {
                // Send result over the associated connection
                conn.send(result);
            };
            // Submit the request to the JSON interface
            auto result = interface.handle(json, cb);
            // Close the associated connection if appropriate
            if (result == JsonInterface::CONCLUDE) conn.close();
        };

        // Create socket object and start listening / processing
        _socket = new Socket(settings);
    }

    ~SocketConnector() {
        // Stop listening / processing, clean up
        delete _socket;
    }
};

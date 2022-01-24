
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
    SocketConnector(Parameters& params, JsonInterface& interface, std::string socketPath) {

        // Settings for the local socket
        Socket::SocketSettings settings;
        settings.address = socketPath;
        settings.maxNumConnections = params.activeJobsPerClient();
        settings.maxMsgSize = 65536;

        // Callback to receive a JSON request from a certain user connection
        settings.receiveCallback = [&interface](Connection& conn, nlohmann::json& json) {
            // Callback to return an answer for the request being submitted
            auto cb = [&conn](nlohmann::json& result) {
                // Send result over the associated connection
                bool sent = conn.send(result);
                if (!sent) LOG(V1_WARN, "[WARN] IPC socket send unsuccessful!\n");
            };
            // Submit the request to the JSON interface
            auto result = interface.handle(json, cb);
            // Close the associated connection if appropriate
            if (result == JsonInterface::ACCEPT_CONCLUDE) conn.close();
        };

        // Create socket object and start listening / processing
        _socket = new Socket(settings);
    }

    ~SocketConnector() {
        // Stop listening / processing, clean up
        delete _socket;
    }
};

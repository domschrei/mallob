
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <string>
#include <optional>

#include "util/logger.hpp"
#include "connection.hpp"
#include "util/sys/background_worker.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/fileutils.hpp"

class Socket {

private:
    std::string _socket_address;
    int _max_num_connections;
    int _max_msg_size;
    std::function<void(Connection&, nlohmann::json&)> _recv_callback;

    int _socket_fd;
    BackgroundWorker _server_thread;
    ThreadPool _thread_pool;
    int _running_conn_id = 1;

public:

    struct SocketSettings {
        std::string address;
        int maxNumConnections; 
        int maxMsgSize;
        std::function<void(Connection&, nlohmann::json&)> receiveCallback;
    };

    Socket(const SocketSettings& settings)
        : _socket_address(settings.address), _max_num_connections(settings.maxNumConnections), 
        _max_msg_size(settings.maxMsgSize), _recv_callback(settings.receiveCallback), 
        _thread_pool(_max_num_connections) {
        
        // Setting my domain
        sockaddr_un address;
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, _socket_address.c_str(), _socket_address.size() + 1); 

        // Make a socket file
        if ((_socket_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
            LOG(V1_WARN, "Could not instantiate socket \"%s\"\n", _socket_address.c_str());
            return;
        }

        // Bind 
        // Naming the socket so other process can address it
        unlink(_socket_address.c_str());
        if (bind(_socket_fd, (sockaddr*) &address, sizeof(address)) == -1) {
            LOG(V1_WARN, "Could not bind socket \"%s\"\n", _socket_address.c_str());
            return;
        }

        // Listen
        // Set the socket as passive for accepting connection
        // On the other side connect() will succeed immediately if
        // the queue is not full.
        if (listen(_socket_fd, _max_num_connections) == -1) {
            LOG(V1_WARN, "Could not bind socket \"%s\"\n", _socket_address.c_str());
            return;
        }

        _server_thread.run([this]() {
            Proc::nameThisThread("SocketServer");
            while (_server_thread.continueRunning()) {

                auto optConn = openConnection();
                if (!optConn) continue;
                if (!optConn.value().valid()) continue;

                Connection* connection = new Connection(std::move(optConn.value()));
                auto task = [this, connection]() {
                    while (true) {
                        if (!connection->valid()) return;
                        auto optJson = connection->receive();
                        if (!optJson) continue;
                        auto json = optJson.value();
                        _recv_callback(*connection, json);
                    }
                    delete connection;
                };
                _thread_pool.addTask(std::move(task));
            }
        });
    }

    ~Socket() {
        // TODO somehow interrupt blocking wait for a connection in _server_thread
        if (_socket_fd != -1) {
            ::close(_socket_fd);
            unlink(_socket_address.c_str());
        } 
    }

    std::optional<Connection> openConnection() {

        sockaddr_un agent; // The other process info
        socklen_t agent_length;
        memset(&agent, 0, sizeof(sockaddr_un));

        int fd = -1;
        if ((fd = accept(_socket_fd, (sockaddr*) &agent, &agent_length)) == -1) {
            LOG(V1_WARN, "Cannot accept connection!\n");
            return std::optional<Connection>();
        }

        printf("Established connection with: %s\n", agent.sun_path); // it may show nothing because the socket is unbound
        return std::optional<Connection>(Connection(_running_conn_id++, fd, _max_msg_size));
    }
};

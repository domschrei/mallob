
#pragma once


#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <optional>

#include "util/json.hpp"

class Connection {

private:
    int _id;
    int _connection_fd;
    int _max_msg_size;

public:
    Connection(int id, int fd, int maxMsgSize) : _id(id), _connection_fd(fd), _max_msg_size(maxMsgSize) {}
    ~Connection() {
        close();
    }
    Connection(Connection&& other) : _id(other._id), _connection_fd(other._connection_fd), _max_msg_size(other._max_msg_size) {
        other._id = -1;
        other._connection_fd = -1;
    }
    Connection& operator=(Connection&& other) {
        *this = Connection(std::move(other));
        return *this;
    }

    bool send(const nlohmann::json& json) {
        if (!valid()) return false;

        std::string serializedJson = json.dump();
        int size = ::send(_connection_fd, serializedJson.c_str(), serializedJson.size(), 0);

        return size >= 0;
    }
    
    std::optional<nlohmann::json> receive() {
        if (!valid()) return std::optional<nlohmann::json>();

        char message[_max_msg_size];
	    message[_max_msg_size-1] = '\0';
        size_t size;
        memset(message, 0, _max_msg_size);
        
        size = recv(_connection_fd, message, _max_msg_size, 0);
        if (size == -1) return std::optional<nlohmann::json>();

        std::string msg = std::string(message, size);
        return std::optional<nlohmann::json>(nlohmann::json::parse(msg));
    }

    bool valid() {
        return _connection_fd != -1;
    }

    void close() {
        if (!valid()) return;
        ::close(_connection_fd);
        _connection_fd = -1;
    }

    int getId() const {
        return _id;
    }
};

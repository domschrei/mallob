
#pragma once


#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <optional>
#include <cctype>
#include <inttypes.h>

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
    Connection(Connection&& other) : _id(other._id), _connection_fd(other._connection_fd), 
            _max_msg_size(other._max_msg_size) {
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

        int payloadSize = serializedJson.size();
        int flippedPayloadSize = flipEndian(payloadSize);
        int msgSize = sizeof(int)+payloadSize;
        char msg[msgSize];
        memcpy(msg, &flippedPayloadSize, sizeof(int));
        memcpy(msg+sizeof(int), serializedJson.c_str(), payloadSize);
        
        int size = ::send(_connection_fd, msg, msgSize, 0);
        log(V5_DEBG, "Sending msg len=%i/%i \"%s\"\n", size, msgSize, serializedJson.c_str());
        return size >= 0;
    }
    
    std::optional<nlohmann::json> receive() {
        if (!valid()) return std::optional<nlohmann::json>();

        char msg[_max_msg_size];
	    msg[_max_msg_size-1] = '\0';
        memset(msg, 0, _max_msg_size-1);
        
        auto size = recv(_connection_fd, msg, _max_msg_size, 0);
        if (size <= 0) return std::optional<nlohmann::json>();

        if (size == sizeof(int)+1 && (int) msg[sizeof(int)] == 24) {
            // Goodbye message
            close();
            return std::optional<nlohmann::json>();
        }

        std::string msgStr = "";
        //std::string fullStringAsInts = "";
        for (int i = sizeof(int); i < size; i++) {
            const char c = msg[i];
            //fullStringAsInts += std::to_string((int) c) + ",";
            if (isprint(c)) msgStr += std::string(1, c);
        }
        //fullStringAsInts = fullStringAsInts.substr(0, fullStringAsInts.size()-1);
        
        log(V5_DEBG, "Received msg len=%i/%i \"%s\"\n", msgStr.size(), size, msgStr.c_str());
        
        try {
            return std::optional<nlohmann::json>(nlohmann::json::parse(msgStr));
        } catch (const nlohmann::detail::parse_error& e) {
            log(V1_WARN, "[WARN] Parse error on \"%s\": %s\n", msgStr.c_str(), e.what());
            return std::optional<nlohmann::json>();
        }
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

private:
    int flipEndian(int input) {
        char in[4];
        memcpy(in, &input, sizeof(int));
        char out[4];
        for (int i = 0; i < 4; i++) {
            out[i] = in[3-i];
        }
        return *((int*)out);
    }

};

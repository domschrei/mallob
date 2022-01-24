
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

    std::vector<char> _buffer;

    bool _flip_endian = false;

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
        int flippedPayloadSize = _flip_endian ? flipEndian(payloadSize) : payloadSize;
        int msgSize = sizeof(int)+payloadSize;
        char msg[msgSize];
        memcpy(msg, &flippedPayloadSize, sizeof(int));
        memcpy(msg+sizeof(int), serializedJson.c_str(), payloadSize);
        
        int size = ::send(_connection_fd, msg, msgSize, 0);
        LOG(V5_DEBG, "Sending msg len=%i/%i \"%s\"\n", size, msgSize, serializedJson.c_str());
        return size >= 0;
    }
    
    std::optional<nlohmann::json> receive() {
        if (!valid()) return std::optional<nlohmann::json>();

        // Remember old buffer size and enlarge
        int sizeBefore = _buffer.size();
        _buffer.resize(sizeBefore + _max_msg_size);
        
        // Attempt to receive a block of message(s)
        auto size = recv(_connection_fd, _buffer.data()+sizeBefore, _max_msg_size, 0);

        // Trim buffer to only contain actual data
        _buffer.resize(sizeBefore + size);

        auto result = std::optional<nlohmann::json>();

        // Received nothing?
        if (size <= 0) return result;

        // Try to read a message from the buffer
        int begin = 0;
        while (true) {

            // How large is the message advertised to be? (Try to be lenient with byte ordering)
            int payloadSize = *((int*) (_buffer.data()+begin));
            if (_flip_endian || payloadSize < 0 || payloadSize >= INT32_MAX/2) {
                payloadSize = flipEndian(payloadSize);
                _flip_endian = true;
            }
            assert(payloadSize >= 0 && payloadSize < INT32_MAX/2);
            
            // Proceed to actual payload
            begin += sizeof(int);

            // Single CANCEL byte?
            if (payloadSize == 1 && (int) _buffer[begin] == 24) {

                // Goodbye message (ignore any following messages)
                close();
                break;

            } else if (payloadSize > _buffer.size()-begin) {

                // Message has not been read completely: wait for next batch
                begin -= sizeof(int);
                break;

            } else {
                // Parse printable ASCII string
                std::string msgStr = "";
                //std::string fullStringAsInts = "";
                for (int i = 0; i < payloadSize; i++) {
                    assert(begin < _buffer.size());
                    const char c = _buffer[begin];
                    //fullStringAsInts += std::to_string((int) c) + ",";
                    if (isprint(c)) msgStr += std::string(1, c);
                    begin++;
                }

                //fullStringAsInts = fullStringAsInts.substr(0, fullStringAsInts.size()-1);
                
                LOG(V5_DEBG, "Received msg len=%i/%i \"%s\"\n", msgStr.size(), size, msgStr.c_str());
                
                // Attempt to parse JSON
                try {
                    auto json = nlohmann::json::parse(msgStr);

                    // Parsing successful!
                    result = json;
                    break;

                } catch (const nlohmann::detail::parse_error& e) {
                    LOG(V1_WARN, "[WARN] Parse error on \"%s\": %s\n", msgStr.c_str(), e.what());
                }
            }
        }

        // Rearrange buffer to begin at beginning of next message
        auto newbuf = std::vector<char>(_buffer.data()+begin, _buffer.data()+_buffer.size());
        _buffer = std::move(newbuf);

        return result;
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

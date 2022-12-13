
#pragma once

#include <vector>
#include <cstring>

#include "comm/mpi_base.hpp"
#include "data/serializable.hpp"

/*
Represents a single message that is being sent or received.
*/
struct MessageHandle {

private:
    std::vector<uint8_t> data;

public:
    int tag;
    int source;

    MessageHandle() = default;
    MessageHandle(const MessageHandle& copied) {
        tag = copied.tag;
        source = copied.source;
        data = copied.data;
    }
    MessageHandle(MessageHandle&& moved) {
        tag = moved.tag;
        source = moved.source;
        data = std::move(moved.data);
    }
    ~MessageHandle() = default;

    MessageHandle& operator=(MessageHandle&& moved) {
        tag = moved.tag;
        source = moved.source;
        data = std::move(moved.data);
        return *this;
    }

    const std::vector<uint8_t>& getRecvData() const { return data;}
    std::vector<uint8_t>&& moveRecvData() { return std::move(data);}

    void setReceive(size_t msgSize, uint8_t* recvData) {
        data.resize(msgSize);
        memcpy(data.data(), recvData, msgSize);
    }
    void setReceive(std::vector<uint8_t>&& recvData) {
        data = std::move(recvData);
    }

    void receiveSelfMessage(const std::vector<uint8_t>& recvData, int rank) {
        receiveSelfMessage(std::vector<uint8_t>(recvData), rank);
    }
    void receiveSelfMessage(std::vector<uint8_t>&& recvData, int rank) {
        data = std::move(recvData);
        source = rank;
    }
};

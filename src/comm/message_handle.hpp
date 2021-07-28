
#ifndef DOMPASCH_MALLOB_MESSAGE_HANDLE_HPP
#define DOMPASCH_MALLOB_MESSAGE_HANDLE_HPP

#include <vector>
#include <memory>

#include "comm/mpi_base.hpp"

/*
Represents a single message that is being sent or received.
*/
struct MessageHandle {

private:
    std::vector<uint8_t> data;

public:
    int tag;
    int source;
    bool selfMessage = false;
    bool finished = false;
    float creationTime = 0;

    MessageHandle() = default;
    MessageHandle(MessageHandle&& moved) {
        tag = moved.tag;
        source = moved.source;
        selfMessage = moved.selfMessage;
        finished = moved.finished;
        creationTime = moved.creationTime;
        data = std::move(moved.data);
    }
    ~MessageHandle() = default;

    MessageHandle& operator=(MessageHandle&& moved) {
        tag = moved.tag;
        source = moved.source;
        selfMessage = moved.selfMessage;
        finished = moved.finished;
        creationTime = moved.creationTime;
        data = std::move(moved.data);
        return *this;
    }

    const std::vector<uint8_t>& getRecvData() const { return data;}
    void setReceive(std::vector<uint8_t>&& buf) {data = std::move(buf);}
    std::vector<uint8_t>&& moveRecvData() { return std::move(data);}

    void receiveSelfMessage(const std::vector<uint8_t>& recvData, int rank) {
        receiveSelfMessage(std::vector<uint8_t>(recvData), rank);
    }
    void receiveSelfMessage(std::vector<uint8_t>&& recvData, int rank) {
        data = std::move(recvData);
        source = rank;
        selfMessage = true;
    }
};

typedef std::unique_ptr<MessageHandle> MessageHandlePtr;

#endif


#include <chrono>
#include <unistd.h>
#include <ctime>
#include <algorithm>
#include <iostream>
#include "util/assert.hpp"

#include "mympi.hpp"

#include "util/params.hpp"
#include "util/random.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/sys/process.hpp"

MessageQueue* MyMpi::_msg_queue;

void MyMpi::init() {
    int provided = -1;
    MPICALL(MPI_Init_thread(nullptr, nullptr, MPI_THREAD_FUNNELED, &provided), std::string("init"))
    if (provided != MPI_THREAD_FUNNELED) {
        std::cout << "[ERROR] MPI: wanted id=" << MPI_THREAD_FUNNELED 
                << ", got id=" << provided << std::endl;
        Process::doExit(1);
    }
}

size_t MyMpi::getBinaryTreeBufferLimit(int numWorkers, int baseSize, float discountFactor, BufferQueryMode mode) {
    float limit = baseSize * std::pow(discountFactor, std::log2(numWorkers+1));
    if (mode == SELF) {
        return std::ceil(limit);
    } else {
        return std::ceil(numWorkers * limit);
    }
}

void MyMpi::setOptions(const Parameters& params) {
    int verb = MyMpi::rank(MPI_COMM_WORLD) == 0 ? V2_INFO : V4_VVER;
    _msg_queue = new MessageQueue(params.messageBatchingThreshold());
}

int MyMpi::isend(int recvRank, int tag, const Serializable& object) {
    return _msg_queue->send(DataPtr(new std::vector<uint8_t>(object.serialize())), recvRank, tag);
}
int MyMpi::isend(int recvRank, int tag, std::vector<uint8_t>&& object) {
    return _msg_queue->send(DataPtr(new std::vector<uint8_t>(std::move(object))), recvRank, tag);
}
int MyMpi::isend(int recvRank, int tag, const DataPtr& object) {
    return _msg_queue->send(object, recvRank, tag);
}
int MyMpi::isendCopy(int recvRank, int tag, const std::vector<uint8_t>& object) {
    return _msg_queue->send(DataPtr(new std::vector<uint8_t>(object)), recvRank, tag);
}

MPI_Request MyMpi::iallreduce(MPI_Comm communicator, float* contribution, float* result, MPI_Op operation) {
    MPI_Request req;
    MPICALL(MPI_Iallreduce(contribution, result, 1, MPI_FLOAT, operation, communicator, &req), std::string("iallreduce"))
    return req;
}

MPI_Request MyMpi::iallreduce(MPI_Comm communicator, float* contribution, float* result, int numFloats, MPI_Op operation) {
    MPI_Request req;
    MPICALL(MPI_Iallreduce(contribution, result, numFloats, MPI_FLOAT, operation, communicator, &req), std::string("iallreduce"));
    return req;
}

MPI_Request MyMpi::ireduce(MPI_Comm communicator, float* contribution, float* result, int rootRank, MPI_Op operation) {
    MPI_Request req;
    MPICALL(MPI_Ireduce(contribution, result, 1, MPI_FLOAT, operation, rootRank, communicator, &req), std::string("ireduce"))
    return req;
}

MPI_Request MyMpi::iallgather(MPI_Comm communicator, float* contribution, float* result, int numFloats) {
    MPI_Request req;
    MPICALL(MPI_Iallgather(contribution, numFloats, MPI_FLOAT, result, numFloats, MPI_FLOAT, communicator, &req), std::string("iallgather"));
    return req;
}

int MyMpi::size(MPI_Comm comm) {
    int size = 0;
    MPICALL(MPI_Comm_size(comm, &size), std::string("commSize"))
    return size;
}

int MyMpi::rank(MPI_Comm comm) {
    int rank = -1;
    MPICALL(MPI_Comm_rank(comm, &rank), std::string("commRank"))
    return rank;
}

MessageQueue& MyMpi::getMessageQueue() {
    return *_msg_queue;
}

/*
void MyMpi::latencyMonkey() {
    if (_monkey_flags & MONKEY_LATENCY) {
        float duration = 1 * 1000 + 9 * 1000 * Random::rand(); // Sleep between one and ten millisecs
        //log(V5_DEBG, "LATENCY_MONKEY %.3fs\n", 0.001 * 0.001 * duration);
        usleep(duration); 
    }
}

void MyMpi::delayMonkey() {
    if ((_monkey_flags & MONKEY_DELAY) && Random::rand() * 100 <= 1) { // chance of 1:100
        float duration = 1000 * 1000 * Random::rand(); // Sleep for up to one second
        LOG(V5_DEBG, "DELAY_MONKEY %.3fs\n", 0.001 * 0.001 * duration);
        usleep(duration); 
    }
}
*/

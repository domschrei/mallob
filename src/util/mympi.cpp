
#include <chrono>
#include <unistd.h>
#include <ctime>
#include <algorithm>

#include "mympi.h"
#include "random.h"
#include "timer.h"
#include "console.h"

#include "util/mpi_monitor.h"

#define MPICALL(cmd, str) {if (!MyMpi::_monitor_off) {initcall((str).c_str());} int err = cmd; if (!MyMpi::_monitor_off) endcall(); chkerr(err);}

int MyMpi::_max_msg_length;
std::set<MessageHandlePtr> MyMpi::_handles;
std::set<MessageHandlePtr> MyMpi::_sent_handles;
std::set<MessageHandlePtr> MyMpi::_deferred_handles;
/*
std::set<MessageHandlePtr, MyMpi::HandleComparator> MyMpi::_handles;
std::set<MessageHandlePtr, MyMpi::HandleComparator> MyMpi::_sent_handles;
std::set<MessageHandlePtr, MyMpi::HandleComparator> MyMpi::_deferred_handles;*/
ListenerMode MyMpi::_mode;
std::map<int, int> MyMpi::_msg_priority;
bool MyMpi::_monitor_off;

void chkerr(int err) {
    if (err != 0) {
        Console::log(Console::CRIT, "MPI ERROR errcode=%i", err);
        abort();
    }
}

bool MessageHandle::testSent() {
    assert(!selfMessage || Console::fail("Attempting to MPI_Test a self message!"));
    if (finished) return true;
    int flag = 0;
    MPICALL(MPI_Test(&request, &flag, &status), "testsent-id" + std::to_string(id))
    if (flag) finished = true;
    return finished;
}

bool MessageHandle::testReceived() {

    // Already finished at some earlier point?
    if (finished) return true;

    // Check if message was received
    if (selfMessage || status.MPI_TAG > 0) {
        // Self message / fabricated message is invariantly ready to be processed
        finished = true;
    } else {
        // MPI_Test message
        int flag = -1;
        MPICALL(MPI_Test(&request, &flag, &status), "testrecvd-id" + std::to_string(id))
        if (flag) {
            finished = true;
            tag = status.MPI_TAG;
            assert(status.MPI_SOURCE >= 0 || Console::fail("MPI_SOURCE = %i", status.MPI_SOURCE));
            source = status.MPI_SOURCE;
        }
    }

    // Resize received data vector to actual received size
    if (finished && !selfMessage) {
        int count = 0;
        MPICALL(MPI_Get_count(&status, MPI_BYTE, &count), "getCount-id" + std::to_string(id))
        if (count > 0 && count < recvData->size()) {
            recvData->resize(count);
        }
    }

    return finished;
}


int MyMpi::nextHandleId() {
    return handleId++;
}

void MyMpi::init(int argc, char *argv[])
{    
    int provided = -1;
    MPICALL(MPI_Init_thread(&argc, &argv, MPI_THREAD_SINGLE, &provided), std::string("init"))
    if (provided != MPI_THREAD_SINGLE) {
        std::cout << "ERROR initializing MPI: wanted id=" << MPI_THREAD_SINGLE 
                << ", got id=" << provided << std::endl;
        exit(1);
    }

    _max_msg_length = MyMpi::size(MPI_COMM_WORLD) * MAX_JOB_MESSAGE_PAYLOAD_PER_NODE + 10;
    _monitor_off = false;
    handleId = 1;
}

void MyMpi::beginListening(const ListenerMode& mode) {

    _mode = mode;

    int i = 0;
    for (int tag : ALL_TAGS) {
        _msg_priority[tag] = i++;
    }

    if (_mode == CLIENT) {
        for (int tag : ANYTIME_CLIENT_RECV_TAGS) {
            MessageHandlePtr handle = MyMpi::irecv(MPI_COMM_WORLD, tag);
            Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", handle->id, tag);
        }
    }
    if (_mode == WORKER) {
        for (int tag : ANYTIME_WORKER_RECV_TAGS) {
            MessageHandlePtr handle = MyMpi::irecv(MPI_COMM_WORLD, tag);
            Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", handle->id, tag);
        }
    }
}

void MyMpi::resetListenerIfNecessary(int tag) {
    // Check if tag should be listened to
    if (!isAnytimeTag(tag)) return;
    // add listener
    MessageHandlePtr handle = MyMpi::irecv(MPI_COMM_WORLD, tag);
    Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", handle->id, tag);
}

bool MyMpi::isAnytimeTag(int tag) {
    if (_mode == CLIENT)
        for (int t : ANYTIME_CLIENT_RECV_TAGS)
            if (tag == t) return true;
    if (_mode == WORKER) 
        for (int t : ANYTIME_WORKER_RECV_TAGS)
            if (tag == t) return true;
    return false;
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    
    float time = Timer::elapsedSeconds();
    std::shared_ptr<std::vector<uint8_t>> vec = object.serialize();
    float timeSerialize = Timer::elapsedSeconds() - time;

    time = Timer::elapsedSeconds();
    MessageHandlePtr handle = isend(communicator, recvRank, tag, vec);
    float timeSend = Timer::elapsedSeconds() - time;

    Console::log(Console::VVVERB, "Msg ID=%i serializeTime=%.4f totalIsendTime=%.4f", handle->id, timeSerialize, timeSend);
    return handle;
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {

    if (object->empty()) {
        object->push_back(0);
    }
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), object));
    handle->tag = tag;

    bool selfMessage = rank(communicator) == recvRank;
    if (selfMessage) {
        handle->recvData = handle->sendData;
        handle->source = recvRank;
        handle->selfMessage = true;
    } else {
        MPICALL(MPI_Isend(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator, &handle->request), std::string("isend"))
    }

    (selfMessage ? _handles : _sent_handles).insert(handle);
    
    Console::log(Console::VVVERB, "Msg ID=%i dest=%i tag=%i size=%i", handle->id, 
                recvRank, tag, handle->sendData->size());
    return handle;
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    return send(communicator, recvRank, tag, object.serialize());
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), object));
    if (rank(communicator) == recvRank) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
        _handles.insert(handle);
    } else {
        MPICALL(MPI_Send(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator), std::string("send"))
    }
    Console::log(Console::VVVERB, "Msg ID=%i sent.", handle->id);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator) {
    return irecv(communicator, MPI_ANY_TAG);
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int tag) {
    return irecv(communicator, MPI_ANY_SOURCE, tag);
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;

    int msgSize;
    if (tag == MSG_JOB_COMMUNICATION || tag == MSG_COLLECTIVES || tag == MPI_ANY_TAG) {
        msgSize = _max_msg_length;
    } else {
        msgSize = MAX_ANYTIME_MESSAGE_SIZE;
    }

    handle->recvData->resize(msgSize);
    MPICALL(MPI_Irecv(handle->recvData->data(), msgSize, MPI_BYTE, source, tag, communicator, &handle->request), std::string("irecv"))
    _handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    assert(source >= 0);
    handle->source = source;
    handle->tag = tag;
    handle->recvData->resize(size);
    MPICALL(MPI_Irecv(handle->recvData->data(), size, MPI_BYTE, source, tag, communicator, &handle->request), std::string("irecv"))
    _handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;
    handle->recvData->resize(size);
    MPICALL(MPI_Recv(handle->recvData->data(), size, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->status), std::string("recv"))
    handle->source = handle->status.MPI_SOURCE;
    int count = 0;
    MPICALL(MPI_Get_count(&handle->status, MPI_BYTE, &count), std::string("getcount"))
    if (count < size) {
        handle->recvData->resize(count);
    }
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag) {
    return recv(communicator, tag, _max_msg_length);
}

MPI_Request MyMpi::iallreduce(MPI_Comm communicator, float* contribution, float* result) {
    MPI_Request req;
    MPICALL(MPI_Iallreduce(contribution, result, 1, MPI_FLOAT, MPI_SUM, communicator, &req), std::string("iallreduce"))
    return req;
}

MPI_Request MyMpi::iallreduce(MPI_Comm communicator, float* contribution, float* result, int numFloats) {
    MPI_Request req;
    MPICALL(MPI_Iallreduce(contribution, result, numFloats, MPI_FLOAT, MPI_SUM, communicator, &req), std::string("iallreduce"));
    return req;
}

MPI_Request MyMpi::ireduce(MPI_Comm communicator, float* contribution, float* result, int rootRank) {
    MPI_Request req;
    MPICALL(MPI_Ireduce(contribution, result, 1, MPI_FLOAT, MPI_SUM, rootRank, communicator, &req), std::string("ireduce"))
    return req;
}

bool MyMpi::test(MPI_Request& request, MPI_Status& status) {
    int flag = 0;
    MPICALL(MPI_Test(&request, &flag, &status), std::string("test"))
    return flag;
}

std::vector<MessageHandlePtr> MyMpi::poll() {

    std::vector<MessageHandlePtr> foundHandles;
    std::vector<bool> handlesDeferred;

    // Find ready handle of best priority
    for (auto& h : _handles) {
        if (h->testReceived()) {
            foundHandles.push_back(h);
            handlesDeferred.push_back(false);
        }
    }

    // If necessary, pick a deferred handle (if there is one)
    if (foundHandles.empty()) {
        for (auto& h : _deferred_handles) {
            if (h->testReceived()) {
                foundHandles.push_back(h);
                handlesDeferred.push_back(true);
            }
        }   
    }

    // Remove and return found handle
    for (int i = 0; i < foundHandles.size(); i++) {
        (handlesDeferred[i] ? _deferred_handles : _handles).erase(foundHandles[i]);
        resetListenerIfNecessary(foundHandles[i]->tag);
    } 
    return foundHandles;
}

void MyMpi::deferHandle(MessageHandlePtr handle) {
    _deferred_handles.insert(handle);
}

bool MyMpi::hasOpenSentHandles() {
    return !_sent_handles.empty();
}

void MyMpi::testSentHandles() {
    std::vector<MessageHandlePtr> finishedHandles;
    for (auto& h : _sent_handles) {
        if (h->testSent()) {
            // Sending operation completed
            Console::log(Console::VVVERB, "Msg ID=%i isent", h->id);
            finishedHandles.push_back(h);
        }
    }
    for (auto& h : finishedHandles) {
        _sent_handles.erase(h);
    }
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

int MyMpi::random_other_node(MPI_Comm comm, const std::set<int>& excludedNodes)
{
    int size = MyMpi::size(comm);

    float r = Random::rand();
    int node = (int) (r * size);

    while (excludedNodes.find(node) != excludedNodes.end()) {
        r = Random::rand();
        node = (int) (r * size);
    }

    return node;
}

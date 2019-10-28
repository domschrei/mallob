
#include <chrono>
#include <unistd.h>
#include <ctime>
#include <algorithm>

#include "mpi.h"
#include "random.h"
#include "timer.h"
#include "console.h"

std::set<MessageHandlePtr> MyMpi::handles;
std::set<MessageHandlePtr> MyMpi::sentHandles;
int MyMpi::maxMsgLength;

void MyMpi::init(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);
    maxMsgLength = MyMpi::size(MPI_COMM_WORLD) * MAX_JOB_MESSAGE_PAYLOAD_PER_NODE + 10;}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    const std::vector<uint8_t> vec = object.serialize();
    return isend(communicator, recvRank, tag, vec);
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::vector<uint8_t>& object) {
    MessageHandlePtr handle(new MessageHandle(object));
    if (rank(communicator) == recvRank) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
        handles.insert(handle);
    } else {
        MPI_Isend(handle->sendData.data(), handle->sendData.size(), MPI_BYTE, recvRank, tag, communicator, &handle->request);
        sentHandles.insert(handle);
    }
    return handle;
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    return send(communicator, recvRank, tag, object.serialize());
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const std::vector<uint8_t>& object) {
    MessageHandlePtr handle(new MessageHandle(object));
    if (rank(communicator) == recvRank) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
        handles.insert(handle);
    } else {
        MPI_Send(handle->sendData.data(), handle->sendData.size(), MPI_BYTE, recvRank, tag, communicator);
    }
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator) {
    MessageHandlePtr handle(new MessageHandle());
    handle->tag = 0;
    handle->recvData.resize(maxMsgLength);
    MPI_Irecv(handle->recvData.data(), maxMsgLength, MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int tag) {
    MessageHandlePtr handle(new MessageHandle());
    handle->tag = tag;
    handle->recvData.resize(maxMsgLength);
    MPI_Irecv(handle->recvData.data(), maxMsgLength, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag) {
    MessageHandlePtr handle(new MessageHandle());
    handle->tag = tag;
    handle->recvData.resize(maxMsgLength);
    MPI_Irecv(handle->recvData.data(), maxMsgLength, MPI_BYTE, source, tag, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle());
    handle->tag = tag;
    handle->recvData.resize(size);
    MPI_Recv(handle->recvData.data(), size, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->status);
    handle->source = handle->status.MPI_SOURCE;
    int count = 0;
    MPI_Get_count(&handle->status, MPI_BYTE, &count);
    if (count < size) {
        handle->recvData.resize(count);
    }
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag) {
    return recv(communicator, tag, maxMsgLength);
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle());
    handle->source = source;
    handle->tag = tag;
    handle->recvData.resize(size);
    handle->critical = true;
    MPI_Irecv(handle->recvData.data(), size, MPI_BYTE, source, tag, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::poll() {
    cleanSentHandles();

    MessageHandlePtr handle = NULL;
    for (auto it = handles.begin(); it != handles.end(); ++it) {
        MessageHandlePtr h = *it;
        if (h->selfMessage) {
            handle = h;
            break;
        }
        int flag = -1;
        MPI_Test(&h->request, &flag, &h->status);
        if (flag) {
            handle = h;
            handle->tag = handle->status.MPI_TAG;
            assert(handle->status.MPI_SOURCE >= 0);
            handle->source = handle->status.MPI_SOURCE;

            // Resize received data vector to actual received size
            int count = 0;
            MPI_Get_count(&handle->status, MPI_BYTE, &count);
            if (count > 0) {
                handle->recvData.resize(count);
            }

            break;
        }
    }
    if (handle != NULL) {
        handles.erase(handle);
    }
    return handle;
}

bool MyMpi::hasCriticalHandles() {
    bool critical = false;
    for (auto it : handles) {
        if (it->critical) {
            critical = true;
            Console::log(Console::VVERB, "Has handle of tag %i (critical: %s)", it->tag, it->critical ? "yes" : "no");
        }
    }

    if (handles.size() > 1) {
        for (auto it : handles) {
            Console::log(Console::VVERB, " -- tag %i", it->tag);
        }
    }

    return critical;
}

void MyMpi::listen() {
    bool critical = false;
    bool nonCritical = false;
    for (auto it : handles) {
        if (it->critical) {
            critical = true;
        } else {
            nonCritical = true;
        }
    }
    if (!critical && !nonCritical)
        MyMpi::irecv(MPI_COMM_WORLD); // add a non-critical 
}

void MyMpi::deferHandle(MessageHandlePtr handle) {
    handles.insert(handle);
}

void MyMpi::cleanSentHandles() {
    for (auto it = sentHandles.begin(); it != sentHandles.end();) {
        MessageHandlePtr h = *it;
        int flag = -1;
        MPI_Test(&h->request, &flag, &h->status);
        if (flag) {
            // Sending operation completed
            it = sentHandles.erase(it);
        } else {
            it++;
        }
    }
}

int MyMpi::size(MPI_Comm comm) {
    int size = 0;
    MPI_Comm_size(comm, &size);
    return size;
}

int MyMpi::rank(MPI_Comm comm) {
    int rank = -1;
    MPI_Comm_rank(comm, &rank);
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
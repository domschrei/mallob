
#include <chrono>
#include <unistd.h>
#include <ctime>
#include <algorithm>

#include "mpi.h"
#include "random.h"

std::set<MessageHandlePtr> MyMpi::handles;
std::set<MessageHandlePtr> MyMpi::sentHandles;
int MyMpi::maxMsgLength;

using namespace std::chrono;
std::chrono::high_resolution_clock::time_point startTime;
/**
 * Returns elapsed time since program start (since MyMpi::init) in seconds.
 */
float elapsed_time() {
    std::chrono::high_resolution_clock::time_point nowTime = high_resolution_clock::now();
    duration<double, std::milli> time_span = nowTime - startTime;
    return time_span.count() / 1000;
}

void MyMpi::init(int argc, char *argv[])
{
    startTime = high_resolution_clock::now();
    MPI_Init(&argc, &argv);

    /*
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_SERIALIZED, &provided);
    if (provided != MPI_THREAD_SERIALIZED) {
        std::cout << "ERROR: MPI Implementation does not support serialized threading." << std::endl;
        exit(1);
    }*/

    maxMsgLength = MyMpi::size(MPI_COMM_WORLD) * (BROADCAST_CLAUSE_INTS_PER_NODE + 1);
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, int object) {
    std::vector<int> vec;
    vec.push_back(object);
    return isend(communicator, recvRank, tag, vec);
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    const std::vector<int> vec = object.serialize();
    return isend(communicator, recvRank, tag, vec);
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::vector<int>& object) {
    MessageHandlePtr handle(new MessageHandle(object));
    if (rank(communicator) == recvRank) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
        handles.insert(handle);
    } else {
        MPI_Isend(handle->sendData.data(), handle->sendData.size(), MPI_INT, recvRank, tag, communicator, &handle->request);
        sentHandles.insert(handle);
    }
    return handle;
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    std::vector<int> vec = object.serialize();
    return send(communicator, recvRank, tag, vec);
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const std::vector<int>& object) {
    MessageHandlePtr handle(new MessageHandle(object));
    if (rank(communicator) == recvRank) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
        handles.insert(handle);
    } else {
        MPI_Send(handle->sendData.data(), handle->sendData.size(), MPI_INT, recvRank, tag, communicator);
    }
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator) {
    MessageHandlePtr handle(new MessageHandle());
    handle->tag = 0;
    handle->recvData.resize(maxMsgLength);
    MPI_Irecv(handle->recvData.data(), maxMsgLength, MPI_INT, MPI_ANY_SOURCE, MPI_ANY_TAG, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int tag) {
    MessageHandlePtr handle(new MessageHandle());
    handle->tag = tag;
    handle->recvData.resize(maxMsgLength);
    MPI_Irecv(handle->recvData.data(), maxMsgLength, MPI_INT, MPI_ANY_SOURCE, tag, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle());
    handle->tag = tag;
    handle->recvData.resize(size);
    MPI_Recv(handle->recvData.data(), size, MPI_INT, MPI_ANY_SOURCE, tag, communicator, &handle->status);
    handle->source = handle->status.MPI_SOURCE;
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle());
    handle->tag = tag;
    handle->recvData.resize(size);
    MPI_Irecv(handle->recvData.data(), size, MPI_INT, MPI_ANY_SOURCE, tag, communicator, &handle->request);
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
            handle->source = handle->status.MPI_SOURCE;

            // Resize received data vector to actual received size
            int count = 0;
            MPI_Get_count(&handle->status, MPI_INT, &count);
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

void MyMpi::log(const char* str)
{
    int rank = -1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    printf("[%3.3f] ", elapsed_time());
    std::cout << "[" << rank << "] " << str << std::endl;
}

void MyMpi::log(std::string str)
{
    log(str.c_str());
}

void MyMpi::log_send(std::string str, int destRank)
{
    log(str + " => [" + std::to_string(destRank) + "]");
}

void MyMpi::log_recv(std::string str, int sourceRank)
{
    log(str + " <= [" + std::to_string(sourceRank) + "]");
}

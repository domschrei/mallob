
#include <chrono>
#include <unistd.h>
#include <ctime>
#include <algorithm>

#include "mympi.h"
#include "random.h"
#include "timer.h"
#include "console.h"

std::set<MessageHandlePtr> MyMpi::handles;
std::set<MessageHandlePtr> MyMpi::sentHandles;
int MyMpi::maxMsgLength;
std::map<int, int> MyMpi::tagPriority;

int handleId;

int MyMpi::nextHandleId() {
    return handleId++;
}

void MyMpi::init(int argc, char *argv[])
{
    /*
    int provided = -1;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
    if (provided != MPI_THREAD_FUNNELED) {
        std::cout << "ERROR initializing MPI: wanted id=" << MPI_THREAD_FUNNELED 
                << " (MPI_THREAD_FUNNELED), got id=" << provided << std::endl;
        exit(1);
    }*/
    MPI_Init(&argc, &argv);

    maxMsgLength = MyMpi::size(MPI_COMM_WORLD) * MAX_JOB_MESSAGE_PAYLOAD_PER_NODE + 10;
    handleId = 1;

    // Very quick messages, and such which are critical for overall system performance
    tagPriority[MSG_FIND_NODE] = 0;
    tagPriority[MSG_WORKER_FOUND_RESULT] = 1;
    tagPriority[MSG_FORWARD_CLIENT_RANK] = 1;
    tagPriority[MSG_JOB_DONE] = 1;
    tagPriority[MSG_COLLECTIVES] = 1;
    
    // Job-internal management messages
    tagPriority[MSG_REQUEST_BECOME_CHILD] = 2;
    tagPriority[MSG_ACCEPT_BECOME_CHILD] = 2;
    tagPriority[MSG_REJECT_BECOME_CHILD] = 2;
    tagPriority[MSG_QUERY_VOLUME] = 2;
    tagPriority[MSG_UPDATE_VOLUME] = 2;
    tagPriority[MSG_NOTIFY_JOB_REVISION] = 2;
    tagPriority[MSG_QUERY_JOB_REVISION_DETAILS] = 2;
    tagPriority[MSG_SEND_JOB_REVISION_DETAILS] = 2;
    tagPriority[MSG_ACK_JOB_REVISION_DETAILS] = 2;

    // Messages inducing bulky amounts of work
    tagPriority[MSG_ACK_ACCEPT_BECOME_CHILD] = 3; // sending a job desc.
    tagPriority[MSG_SEND_JOB_DESCRIPTION] = 3; // receiving a job desc.
    tagPriority[MSG_QUERY_JOB_RESULT] = 3; // sending a job result
    tagPriority[MSG_SEND_JOB_RESULT] = 3; // receiving a job result

    // Termination and interruption
    tagPriority[MSG_TERMINATE] = 4;
    tagPriority[MSG_INTERRUPT] = 4;
    tagPriority[MSG_ABORT] = 4;
    tagPriority[MSG_WORKER_DEFECTING] = 4;

    // Job-specific communication: not critical for balancing 
    tagPriority[MSG_JOB_COMMUNICATION] = 5;

    /*
    for (int tag : ANYTIME_WORKER_RECV_TAGS) {
        msgBuffers[tag] = std::make_shared<std::vector<uint8_t>>(maxMsgLength);
    }
    for (int tag : ANYTIME_CLIENT_RECV_TAGS) {
        msgBuffers[tag] = std::make_shared<std::vector<uint8_t>>(maxMsgLength);
    }*/
}

void MyMpi::beginListening(const ListenerMode& mode) {
    if (mode == CLIENT) {
        for (int tag : ANYTIME_CLIENT_RECV_TAGS)
            MyMpi::irecv(MPI_COMM_WORLD, tag);
    }
    if (mode == WORKER) {
        for (int tag : ANYTIME_WORKER_RECV_TAGS)
            MyMpi::irecv(MPI_COMM_WORLD, tag);
    }
}

void MyMpi::resetListenerIfNecessary(const ListenerMode& mode, int tag) {
    if (mode == CLIENT) {
        for (int t : ANYTIME_CLIENT_RECV_TAGS)
            if (tag == t) {
                MyMpi::irecv(MPI_COMM_WORLD, tag);
                return;
            }
    }
    if (mode == WORKER) {
        for (int t : ANYTIME_WORKER_RECV_TAGS)
            if (tag == t) {
                MyMpi::irecv(MPI_COMM_WORLD, tag);
                return;
            }
    }
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    std::shared_ptr<std::vector<uint8_t>> vec = object.serialize();
    return isend(communicator, recvRank, tag, vec);
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), object));
    if (rank(communicator) == recvRank) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
        handles.insert(handle);
    } else {
        MPI_Isend(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator, &handle->request);
        sentHandles.insert(handle);
    }
    Console::log(Console::VVVERB, "Msg ID=%i", handle->id);
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
        handles.insert(handle);
    } else {
        MPI_Send(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator);
    }
    Console::log(Console::VVVERB, "Msg ID=%i", handle->id);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = 0;
    handle->recvData->resize(maxMsgLength);
    MPI_Irecv(handle->recvData->data(), maxMsgLength, MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int tag) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;
    handle->recvData->resize(maxMsgLength);
    MPI_Irecv(handle->recvData->data(), maxMsgLength, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;
    handle->recvData->resize(maxMsgLength);
    MPI_Irecv(handle->recvData->data(), maxMsgLength, MPI_BYTE, source, tag, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;
    handle->recvData->resize(size);
    MPI_Recv(handle->recvData->data(), size, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->status);
    handle->source = handle->status.MPI_SOURCE;
    int count = 0;
    MPI_Get_count(&handle->status, MPI_BYTE, &count);
    if (count < size) {
        handle->recvData->resize(count);
    }
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag) {
    return recv(communicator, tag, maxMsgLength);
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    assert(source >= 0);
    handle->source = source;
    handle->tag = tag;
    handle->recvData->resize(size);
    handle->critical = true;
    MPI_Irecv(handle->recvData->data(), size, MPI_BYTE, source, tag, communicator, &handle->request);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::poll() {
    testSentHandles();

    /*
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
    */

    MessageHandlePtr bestPrioHandle = NULL;
    int bestPrio = 9999999;

    // Find ready handle of best priority
    for (auto it = handles.begin(); it != handles.end(); ++it) {
        MessageHandlePtr h = *it;
        bool consider = false;
        if (h->selfMessage || h->status.MPI_TAG > 0) {
            // Message is already ready to be processed
            consider = true;
        } else {
            int flag = -1;
            int err = MPI_Test(&h->request, &flag, &h->status);
            assert(err == 0 || Console::fail("MPI ERROR: %i", err));
            if (flag) {
                consider = true;
                h->tag = h->status.MPI_TAG;
                assert(h->status.MPI_SOURCE >= 0 || Console::fail("MPI_SOURCE = %i", h->status.MPI_SOURCE));
                h->source = h->status.MPI_SOURCE;
            }
        }
        if (consider && tagPriority[h->tag] < bestPrio) {
            bestPrio = tagPriority[h->tag];
            bestPrioHandle = h;
            if (bestPrio == MIN_PRIORITY) break; // handle of minimum priority found
            //break;
        }
    }

    // Process found handle
    MessageHandlePtr handle = bestPrioHandle;
    if (handle != NULL && !handle->selfMessage) {
        // Resize received data vector to actual received size
        int count = 0;
        MPI_Get_count(&handle->status, MPI_BYTE, &count);
        if (count > 0) {
            handle->recvData->resize(count);
        }
    }
    if (handle != NULL) handles.erase(handle);
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

void MyMpi::deferHandle(MessageHandlePtr handle) {
    handles.insert(handle);
}

void MyMpi::testSentHandles() {
    for (auto it = sentHandles.begin(); it != sentHandles.end();) {
        MessageHandlePtr h = *it;
        int flag = -1;
        MPI_Test(&h->request, &flag, &h->status);
        if (flag) {
            // Sending operation completed
            Console::log(Console::VVVERB, "Message of ID %i successfully sent.", h->id);
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


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
double doingMpiTasksTime;

int MyMpi::nextHandleId() {
    return handleId++;
}

double MyMpi::currentCallStart() {
    return doingMpiTasksTime;
}

void chkerr(int err) {
    if (err != 0) {
        Console::log(Console::CRIT, "MPI ERROR errcode=%i", err);
        abort();
    }
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
    int err = MPI_Init(&argc, &argv);
    chkerr(err);

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

    tagPriority[MSG_WARMUP] = 6;

    /*
    for (int tag : ANYTIME_WORKER_RECV_TAGS) {
        msgBuffers[tag] = std::make_shared<std::vector<uint8_t>>(maxMsgLength);
    }
    for (int tag : ANYTIME_CLIENT_RECV_TAGS) {
        msgBuffers[tag] = std::make_shared<std::vector<uint8_t>>(maxMsgLength);
    }*/

    doingMpiTasksTime = 0;
}

void MyMpi::beginListening(const ListenerMode& mode) {
    doingMpiTasksTime = Timer::elapsedSeconds();
    if (mode == CLIENT) {
        for (int tag : ANYTIME_CLIENT_RECV_TAGS)
            MyMpi::irecv(MPI_COMM_WORLD, tag);
    }
    if (mode == WORKER) {
        for (int tag : ANYTIME_WORKER_RECV_TAGS)
            MyMpi::irecv(MPI_COMM_WORLD, tag);
    }
    doingMpiTasksTime = 0;
}

void MyMpi::resetListenerIfNecessary(const ListenerMode& mode, int tag) {
    doingMpiTasksTime = Timer::elapsedSeconds();
    // Check if tag should be listened to
    bool listen = false;
    if (mode == CLIENT) {
        for (int t : ANYTIME_CLIENT_RECV_TAGS)
            if (tag == t) {
                listen = true;
            }
    }
    if (mode == WORKER) {
        for (int t : ANYTIME_WORKER_RECV_TAGS)
            if (tag == t) {
                listen = true;
            }
    }
    if (!listen) return;
    // Is the tag already being listened to?
    for (auto handle : handles) {
        if (handle->tag == tag) return;
    }
    // No: add listener
    MyMpi::irecv(MPI_COMM_WORLD, tag);
    doingMpiTasksTime = 0;
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    
    doingMpiTasksTime = Timer::elapsedSeconds();
    float time = Timer::elapsedSeconds();
    std::shared_ptr<std::vector<uint8_t>> vec = object.serialize();
    float timeSerialize = Timer::elapsedSeconds() - time;

    time = Timer::elapsedSeconds();
    MessageHandlePtr handle = isend(communicator, recvRank, tag, vec);
    float timeSend = Timer::elapsedSeconds() - time;

    Console::log(Console::VVVERB, "Msg ID=%i serializeTime=%.6f totalSendTime=%.6f", handle->id, timeSerialize, timeSend);
    doingMpiTasksTime = 0;
    return handle;
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {

    doingMpiTasksTime = Timer::elapsedSeconds();
    float time = Timer::elapsedSeconds();
    float startTime = Timer::elapsedSeconds();
    if (object->empty()) {
        object->push_back(0);
    }
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), object));
    float timeCreateHandle = Timer::elapsedSeconds() - time;

    time = Timer::elapsedSeconds();
    bool selfMessage = rank(communicator) == recvRank;
    if (selfMessage) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
    } else {
        int err = MPI_Isend(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator, &handle->request);
        chkerr(err);
    }
    float timeSend = Timer::elapsedSeconds() - time;

    time = Timer::elapsedSeconds();
    (selfMessage ? handles : sentHandles).insert(handle);
    float timeInsertHandle = Timer::elapsedSeconds() - time;

    Console::log(Console::VVVERB, "Msg ID=%i dest=%i tag=%i starttime=%.6f createHandleTime=%.6f sendTime=%.6f insertHandleTime=%.6f", handle->id, 
                recvRank, tag, startTime, timeCreateHandle, timeSend, timeInsertHandle);
    doingMpiTasksTime = 0;
    return handle;
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    return send(communicator, recvRank, tag, object.serialize());
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {
    doingMpiTasksTime = Timer::elapsedSeconds();
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), object));
    if (rank(communicator) == recvRank) {
        handle->recvData = handle->sendData;
        handle->tag = tag;
        handle->source = recvRank;
        handle->selfMessage = true;
        handles.insert(handle);
    } else {
        int err = MPI_Send(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator);
        chkerr(err);
    }
    Console::log(Console::VVVERB, "Msg ID=%i sent.", handle->id);
    doingMpiTasksTime = 0;
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator) {
    doingMpiTasksTime = Timer::elapsedSeconds();
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = 0;
    handle->recvData->resize(maxMsgLength);
    int err = MPI_Irecv(handle->recvData->data(), maxMsgLength, MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, communicator, &handle->request);
    chkerr(err);
    handles.insert(handle);
    doingMpiTasksTime = 0;
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int tag) {
    doingMpiTasksTime = Timer::elapsedSeconds();
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;

    int msgSize;
    if (tag == MSG_JOB_COMMUNICATION || tag == MSG_COLLECTIVES) {
        msgSize = maxMsgLength;
    } else {
        msgSize = 100;
    }

    handle->recvData->resize(msgSize);
    int err = MPI_Irecv(handle->recvData->data(), msgSize, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->request);
    chkerr(err);
    handles.insert(handle);
    doingMpiTasksTime = 0;
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag) {
    doingMpiTasksTime = Timer::elapsedSeconds();
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;
    handle->recvData->resize(maxMsgLength);
    int err = MPI_Irecv(handle->recvData->data(), maxMsgLength, MPI_BYTE, source, tag, communicator, &handle->request);
    chkerr(err);
    handles.insert(handle);
    doingMpiTasksTime = 0;
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag, int size) {
    doingMpiTasksTime = Timer::elapsedSeconds();
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;
    handle->recvData->resize(size);
    int err = MPI_Recv(handle->recvData->data(), size, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->status);
    chkerr(err);
    handle->source = handle->status.MPI_SOURCE;
    int count = 0;
    err = MPI_Get_count(&handle->status, MPI_BYTE, &count);
    chkerr(err);
    if (count < size) {
        handle->recvData->resize(count);
    }
    doingMpiTasksTime = 0;
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag) {
    return recv(communicator, tag, maxMsgLength);
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag, int size) {
    doingMpiTasksTime = Timer::elapsedSeconds();
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    assert(source >= 0);
    handle->source = source;
    handle->tag = tag;
    handle->recvData->resize(size);
    handle->critical = true;
    int err = MPI_Irecv(handle->recvData->data(), size, MPI_BYTE, source, tag, communicator, &handle->request);
    chkerr(err);
    handles.insert(handle);
    doingMpiTasksTime = 0;
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

    doingMpiTasksTime = Timer::elapsedSeconds();
    MessageHandlePtr bestPrioHandle = NULL;
    int bestPrio = 9999999;

    // Find ready handle of best priority
    for (auto h : handles) {
        bool consider = false;
        if (h->selfMessage || h->status.MPI_TAG > 0) {
            // Message is already ready to be processed
            consider = true;
        } else {
            int flag = -1;
            int err = MPI_Test(&h->request, &flag, &h->status);
            chkerr(err);
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
        int err = MPI_Get_count(&handle->status, MPI_BYTE, &count);
        chkerr(err);
        if (count > 0) {
            handle->recvData->resize(count);
        }
    }
    if (handle != NULL) handles.erase(handle);
    doingMpiTasksTime = 0;
    return handle;
}

/*
template <class T, std::size_t N>
constexpr std::size_t arrsize(const T (&array)[N]) noexcept
{
	return N;
}

MyMpi::pollByProbing(const ListenerMode& mode) {
    cleanSentHandles();

    MessageHandlePtr foundHandle = NULL;

    // Get self message, if present
    for (auto it = handles.begin(); it != handles.end(); ++it) {
        MessageHandlePtr h = *it;
        if (h->selfMessage) {
            foundHandle = h;
            break; // handle of minimum priority found
        }
    }

    const int* tags = (mode == CLIENT ? ANYTIME_CLIENT_RECV_TAGS : ANYTIME_WORKER_RECV_TAGS);
    int size;
    if (mode == CLIENT) size = arrsize(ANYTIME_CLIENT_RECV_TAGS);
    else size = arrsize(ANYTIME_WORKER_RECV_TAGS);

    for (int i = 0; i < size; i++) {
        int tag = tags[i];
        MPI_Pro
    }
}*/

void MyMpi::deferHandle(MessageHandlePtr handle) {
    handles.insert(handle);
}

bool MyMpi::hasOpenSentHandles() {
    return !sentHandles.empty();
}

void MyMpi::testSentHandles() {
    doingMpiTasksTime = Timer::elapsedSeconds();
    for (auto it = sentHandles.begin(); it != sentHandles.end();) {
        MessageHandlePtr h = *it;
        int flag = -1;
        int err = MPI_Test(&h->request, &flag, /*&h->status*/ MPI_STATUS_IGNORE);
        chkerr(err);
        if (flag) {
            // Sending operation completed
            Console::log(Console::VVVERB, "Msg ID=%i isent", h->id);
            it = sentHandles.erase(it);
        } else {
            it++;
        }
    }
    doingMpiTasksTime = 0;
}

int MyMpi::size(MPI_Comm comm) {
    int size = 0;
    int err = MPI_Comm_size(comm, &size);
    chkerr(err);
    return size;
}

int MyMpi::rank(MPI_Comm comm) {
    int rank = -1;
    int err = MPI_Comm_rank(comm, &rank);
    chkerr(err);
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

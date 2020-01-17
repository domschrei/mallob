
#include <chrono>
#include <unistd.h>
#include <ctime>
#include <algorithm>

#include "utilities/Threading.h"
#include "mympi.h"
#include "random.h"
#include "timer.h"
#include "console.h"

std::set<MessageHandlePtr> MyMpi::handles;
std::set<MessageHandlePtr> MyMpi::sentHandles;
int MyMpi::maxMsgLength;
std::map<int, int> MyMpi::tagPriority;

int handleId;
Mutex callLock;
double doingMpiTasksTime;
std::string currentOp;

void initcall(const char* op) {
    callLock.lock();
    currentOp = op;
    doingMpiTasksTime = Timer::elapsedSeconds();
    callLock.unlock();
}
void endcall() {
    callLock.lock();
    doingMpiTasksTime = 0;
    currentOp = "";
    callLock.unlock();
}
std::string MyMpi::currentCall(double* callStart) {
    callLock.lock();
    *callStart = doingMpiTasksTime;
    std::string op = currentOp;
    callLock.unlock();
    return op;
}

void chkerr(int err) {
    if (err != 0) {
        Console::log(Console::CRIT, "MPI ERROR errcode=%i", err);
        abort();
    }
}



bool MessageHandle::testSent() {
    if (finished) return true;
    int flag = 0;
    initcall("test");
    int err = MPI_Test(&request, &flag, &status);
    endcall();
    chkerr(err);
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
        initcall("test");
        int err = MPI_Test(&request, &flag, &status);
        endcall();
        chkerr(err);
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
        initcall("getCount");
        int err = MPI_Get_count(&status, MPI_BYTE, &count);
        endcall();
        chkerr(err);
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
    /*
    int provided = -1;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided);
    if (provided != MPI_THREAD_FUNNELED) {
        std::cout << "ERROR initializing MPI: wanted id=" << MPI_THREAD_FUNNELED 
                << " (MPI_THREAD_FUNNELED), got id=" << provided << std::endl;
        exit(1);
    }*/
    initcall("init");
    int err = MPI_Init(&argc, &argv);
    endcall();
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
    if (!listen) {
        return;
    } 
    // Is the tag already being listened to?
    for (auto handle : handles) {
        if (handle->tag == tag) {
            return;
        }
    }
    // No: add listener
    MyMpi::irecv(MPI_COMM_WORLD, tag);
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    
    float time = Timer::elapsedSeconds();
    std::shared_ptr<std::vector<uint8_t>> vec = object.serialize();
    float timeSerialize = Timer::elapsedSeconds() - time;

    time = Timer::elapsedSeconds();
    MessageHandlePtr handle = isend(communicator, recvRank, tag, vec);
    float timeSend = Timer::elapsedSeconds() - time;

    Console::log(Console::VVVERB, "Msg ID=%i serializeTime=%.6f totalSendTime=%.6f", handle->id, timeSerialize, timeSend);
    return handle;
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {

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
        initcall("isend");
        int err = MPI_Isend(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator, &handle->request);
        endcall();
        chkerr(err);
    }
    float timeSend = Timer::elapsedSeconds() - time;

    time = Timer::elapsedSeconds();
    (selfMessage ? handles : sentHandles).insert(handle);
    float timeInsertHandle = Timer::elapsedSeconds() - time;

    Console::log(Console::VVVERB, "Msg ID=%i dest=%i tag=%i starttime=%.6f createHandleTime=%.6f sendTime=%.6f insertHandleTime=%.6f", handle->id, 
                recvRank, tag, startTime, timeCreateHandle, timeSend, timeInsertHandle);
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
        initcall("send");
        int err = MPI_Send(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator);
        endcall();
        chkerr(err);
    }
    Console::log(Console::VVVERB, "Msg ID=%i sent.", handle->id);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = 0;
    handle->recvData->resize(maxMsgLength);
    initcall("irecv");
    int err = MPI_Irecv(handle->recvData->data(), maxMsgLength, MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, communicator, &handle->request);
    endcall();
    chkerr(err);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int tag) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;

    int msgSize;
    if (tag == MSG_JOB_COMMUNICATION || tag == MSG_COLLECTIVES) {
        msgSize = maxMsgLength;
    } else {
        msgSize = 100;
    }

    handle->recvData->resize(msgSize);
    initcall("irecv");
    int err = MPI_Irecv(handle->recvData->data(), msgSize, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->request);
    endcall();
    chkerr(err);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;
    handle->recvData->resize(maxMsgLength);
    initcall("irecv");
    int err = MPI_Irecv(handle->recvData->data(), maxMsgLength, MPI_BYTE, source, tag, communicator, &handle->request);
    endcall();
    chkerr(err);
    handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId()));
    handle->tag = tag;
    handle->recvData->resize(size);
    initcall("recv");
    int err = MPI_Recv(handle->recvData->data(), size, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->status);
    endcall();
    chkerr(err);
    handle->source = handle->status.MPI_SOURCE;
    int count = 0;
    err = MPI_Get_count(&handle->status, MPI_BYTE, &count);
    chkerr(err);
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
    initcall("irecv");
    int err = MPI_Irecv(handle->recvData->data(), size, MPI_BYTE, source, tag, communicator, &handle->request);
    endcall();
    chkerr(err);
    handles.insert(handle);
    return handle;
}

MPI_Request MyMpi::iallreduce(MPI_Comm communicator, float* contribution, float* result) {
    MPI_Request req;
    initcall("iallreduce");
    MPI_Iallreduce(contribution, result, 1, MPI_FLOAT, MPI_SUM, communicator, &req);
    endcall();
    return req;
}

MPI_Request MyMpi::iallreduce(MPI_Comm communicator, float* contribution, float* result, int numFloats) {
    MPI_Request req;
    initcall("iallreduce");
    MPI_Iallreduce(contribution, result, numFloats, MPI_FLOAT, MPI_SUM, communicator, &req);
    endcall();
    return req;
}

MPI_Request MyMpi::ireduce(MPI_Comm communicator, float* contribution, float* result, int rootRank) {
    MPI_Request req;
    initcall("ireduce");
    MPI_Ireduce(contribution, result, 1, MPI_FLOAT, MPI_SUM, rootRank, communicator, &req);
    endcall();
    return req;
}

MessageHandlePtr MyMpi::poll() {
    testSentHandles();

    MessageHandlePtr bestPrioHandle = NULL;
    int bestPrio = 9999999;

    // Find ready handle of best priority
    for (auto h : handles) {
        if (h->testReceived() && tagPriority[h->tag] < bestPrio) {
            bestPrio = tagPriority[h->tag];
            bestPrioHandle = h;
            if (bestPrio == MIN_PRIORITY) break; // handle of minimum priority found
        }
    }

    // Remove and return found handle
    if (bestPrioHandle != NULL) handles.erase(bestPrioHandle);
    return bestPrioHandle;
}

void MyMpi::deferHandle(MessageHandlePtr handle) {
    handles.insert(handle);
}

bool MyMpi::hasOpenSentHandles() {
    return !sentHandles.empty();
}

void MyMpi::testSentHandles() {
    std::set<MessageHandlePtr> finishedHandles;
    for (auto h : sentHandles) {
        if (h->testSent()) {
            // Sending operation completed
            Console::log(Console::VVVERB, "Msg ID=%i isent", h->id);
            finishedHandles.insert(h);
        }
    }
    for (auto h : finishedHandles) {
        sentHandles.erase(h);
    }
}

int MyMpi::size(MPI_Comm comm) {
    int size = 0;
    initcall("commSize");
    int err = MPI_Comm_size(comm, &size);
    endcall();
    chkerr(err);
    return size;
}

int MyMpi::rank(MPI_Comm comm) {
    int rank = -1;
    initcall("commRank");
    int err = MPI_Comm_rank(comm, &rank);
    endcall();
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

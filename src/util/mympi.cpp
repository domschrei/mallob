
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
std::map<int, MsgTag> MyMpi::_tags;
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
    MPICALL(MPI_Test(&request, &flag, &status), "testsent" + std::to_string(id))
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
        MPICALL(MPI_Test(&request, &flag, &status), "testrecvd" + std::to_string(id))
        if (flag) {
            finished = true;
            tag = status.MPI_TAG;
            assert(status.MPI_SOURCE >= 0 || Console::fail("MPI_SOURCE = %i", status.MPI_SOURCE));
            source = status.MPI_SOURCE;
        }
    }

    // Resize received data vector to actual received size
    if (finished && !selfMessage) {
        if (!selfMessage) {
            int count = 0;
            MPICALL(MPI_Get_count(&status, MPI_BYTE, &count), "getcount" + std::to_string(id))
            if (count > 0 && count < recvData->size()) {
                recvData->resize(count);
            }
        }
        if (tag == MSG_ANYTIME) {
            // Read msg tag of application layer and shrink data by its size
            memcpy(&tag, recvData->data()+recvData->size()-sizeof(int), sizeof(int));
            recvData->resize(recvData->size()-sizeof(int));
            Console::log(Console::VVVERB, "TAG %i\n", tag);
        }
    }

    return finished;
}

bool MessageHandle::shouldCancel(float elapsedTime) {
    // Non-finished, no self message,
    // At least 2 minutes old, not an anytime tag
    if (!finished && !selfMessage && elapsedTime-creationTime > 120.f 
            && !MyMpi::isAnytimeTag(tag)) {
        return true;
    }
    return false;
}

void MessageHandle::cancel() {
    MPICALL(MPI_Cancel(&request), "cancel" + std::to_string(id))
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

    std::vector<MsgTag> tagList;
    /*                   Tag name                          anytime  */
    tagList.emplace_back(MSG_ABORT,                        true); 
    tagList.emplace_back(MSG_ACCEPT_ADOPTION_OFFER,        true); 
    tagList.emplace_back(MSG_ACK_JOB_REVISION_DETAILS,     true);
    tagList.emplace_back(MSG_ANYTIME_REDUCTION,            true);
    tagList.emplace_back(MSG_ANYTIME_BROADCAST,            true);
    tagList.emplace_back(MSG_COLLECTIVES,                  false);
    tagList.emplace_back(MSG_CONFIRM_ADOPTION,             true); 
    tagList.emplace_back(MSG_CLIENT_FINISHED,              true);
    tagList.emplace_back(MSG_EXIT,                         true);   
    tagList.emplace_back(MSG_FIND_NODE,                    true);
    tagList.emplace_back(MSG_FIND_NODE_ONESHOT,            true);
    tagList.emplace_back(MSG_FORWARD_CLIENT_RANK,          true);
    tagList.emplace_back(MSG_INCREMENTAL_JOB_FINISHED,     true);
    tagList.emplace_back(MSG_INTERRUPT,                    true); 
    tagList.emplace_back(MSG_JOB_COMMUNICATION,            true); 
    tagList.emplace_back(MSG_JOB_DONE,                     true);
    tagList.emplace_back(MSG_NOTIFY_JOB_REVISION,          true); 
    tagList.emplace_back(MSG_OFFER_ADOPTION,               true); 
    tagList.emplace_back(MSG_ONESHOT_DECLINED,             true); 
    tagList.emplace_back(MSG_QUERY_JOB_RESULT,             true); 
    tagList.emplace_back(MSG_QUERY_JOB_REVISION_DETAILS,   true); 
    tagList.emplace_back(MSG_QUERY_VOLUME,                 true); 
    tagList.emplace_back(MSG_REJECT_ADOPTION_OFFER,        true); 
    tagList.emplace_back(MSG_RESULT_OBSOLETE,              true);
    tagList.emplace_back(MSG_SEND_JOB_DESCRIPTION,         false); 
    tagList.emplace_back(MSG_SEND_JOB_RESULT,              false); 
    tagList.emplace_back(MSG_SEND_JOB_REVISION_DATA,       false);
    tagList.emplace_back(MSG_SEND_JOB_REVISION_DETAILS,    true); 
    tagList.emplace_back(MSG_TERMINATE,                    true); 
    tagList.emplace_back(MSG_UPDATE_VOLUME,                true); 
    tagList.emplace_back(MSG_WARMUP,                       true); 
    tagList.emplace_back(MSG_WORKER_FOUND_RESULT,          true);
    tagList.emplace_back(MSG_WORKER_DEFECTING,             true); 
    
    for (const auto& tag : tagList) _tags[tag.id] = tag;
}

void MyMpi::beginListening() {

    MessageHandlePtr handle = MyMpi::irecv(MPI_COMM_WORLD, MSG_ANYTIME);
    Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", handle->id, MSG_ANYTIME);
}

void MyMpi::resetListenerIfNecessary(int tag) {
    // Check if tag should be listened to
    if (!isAnytimeTag(tag)) return;
    for (auto& handle : _handles) if (handle->tag == tag) return;
    // add listener
    MessageHandlePtr handle = MyMpi::irecv(MPI_COMM_WORLD, MSG_ANYTIME);
    Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", handle->id, MSG_ANYTIME);
}

bool MyMpi::isAnytimeTag(int tag) {
    if (tag == MSG_ANYTIME) return true;
    assert(_tags.count(tag) || Console::fail("Unknown tag %i\n", tag));
    return _tags[tag].anytime;
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    
    std::shared_ptr<std::vector<uint8_t>> vec = object.serialize();
    MessageHandlePtr handle = isend(communicator, recvRank, tag, vec);
    return handle;
}

MessageHandlePtr MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {

    if (object->empty()) {
        object->push_back(0);
    }
    if (isAnytimeTag(tag)) {
        // Append application layer msg tag to message
        int prevSize = object->size();
        object->resize(prevSize+sizeof(int));
        memcpy(object->data()+prevSize, &tag, sizeof(int));
        tag = MSG_ANYTIME;
    }
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), object));
    handle->tag = tag;

    bool selfMessage = rank(communicator) == recvRank;
    if (selfMessage) {
        handle->recvData = handle->sendData;
        handle->source = recvRank;
        handle->selfMessage = true;
    } else {
        MPICALL(MPI_Isend(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator, &handle->request), "isend"+std::to_string(handle->id))
    }

    (selfMessage ? _handles : _sent_handles).insert(handle);
    
    Console::log(Console::VVVERB, "Msg ID=%i dest=%i tag=%i size=%i", handle->id, 
                recvRank, tag, handle->sendData->size());
    return handle;
}

/*
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
        MPICALL(MPI_Send(handle->sendData->data(), handle->sendData->size(), MPI_BYTE, recvRank, tag, communicator), "send"+std::to_string(handle->id))
    }
    Console::log(Console::VVVERB, "Msg ID=%i sent", handle->id);
    return handle;
}
*/

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator) {
    return irecv(communicator, MPI_ANY_TAG);
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int tag) {
    return irecv(communicator, MPI_ANY_SOURCE, tag);
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag) {

    int msgSize;
    if (tag == MSG_JOB_COMMUNICATION || tag == MSG_COLLECTIVES || tag == MPI_ANY_TAG) {
        msgSize = _max_msg_length;
    } else {
        msgSize = MAX_ANYTIME_MESSAGE_SIZE;
    }

    MessageHandlePtr handle(new MessageHandle(nextHandleId(), msgSize));
    handle->tag = tag;

    MPICALL(MPI_Irecv(handle->recvData->data(), msgSize, MPI_BYTE, source, isAnytimeTag(tag) ? MSG_ANYTIME : tag, 
                communicator, &handle->request), "irecv"+std::to_string(handle->id))
    _handles.insert(handle);
    return handle;
}

MessageHandlePtr MyMpi::irecv(MPI_Comm communicator, int source, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), size));
    assert(source >= 0);
    handle->source = source;
    handle->tag = tag;
    MPICALL(MPI_Irecv(handle->recvData->data(), size, MPI_BYTE, source, isAnytimeTag(tag) ? MSG_ANYTIME : tag, 
                communicator, &handle->request), "irecv"+std::to_string(handle->id))
    _handles.insert(handle);
    return handle;
}

/*
MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag, int size) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), size));
    handle->tag = tag;
    MPICALL(MPI_Recv(handle->recvData->data(), size, MPI_BYTE, MPI_ANY_SOURCE, tag, communicator, &handle->status), "recv"+std::to_string(handle->id))
    handle->source = handle->status.MPI_SOURCE;
    int count = 0;
    MPICALL(MPI_Get_count(&handle->status, MPI_BYTE, &count), "getcount"+std::to_string(handle->id))
    if (count < size) {
        handle->recvData->resize(count);
    }
    return handle;
}

MessageHandlePtr MyMpi::recv(MPI_Comm communicator, int tag) {
    return recv(communicator, tag, _max_msg_length);
}
*/

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
    std::vector<MessageHandlePtr> handlesToCancel;

    float elapsedTime = Timer::elapsedSeconds();

    // Find ready handle of best priority
    for (auto& h : _handles) {
        if (h->testReceived()) {
            foundHandles.push_back(h);
        } else if (h->shouldCancel(elapsedTime)) {
            handlesToCancel.push_back(h);
        }
    }

    // Cancel obsolete handles
    for (int i = 0; i < handlesToCancel.size(); i++) {
        _handles.erase(handlesToCancel[i]);
        handlesToCancel[i]->cancel();
    }

    // Remove and return found handle
    for (int i = 0; i < foundHandles.size(); i++) {
        _handles.erase(foundHandles[i]);
        resetListenerIfNecessary(foundHandles[i]->tag);
    } 

    return foundHandles;
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

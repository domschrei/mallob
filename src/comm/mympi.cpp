
#include <chrono>
#include <unistd.h>
#include <ctime>
#include <algorithm>

#include "mympi.hpp"

#include "util/random.hpp"
#include "util/sys/timer.hpp"
#include "util/console.hpp"
#include "comm/mpi_monitor.hpp"

#define MPICALL(cmd, str) {if (!MyMpi::_monitor_off) {initcall((str).c_str());} \
int err = cmd; if (!MyMpi::_monitor_off) endcall(); chkerr(err);}

int MyMpi::_max_msg_length;
std::vector<MyMpi::MessageHandlePtr> MyMpi::_handles;
std::vector<MyMpi::MessageHandlePtr> MyMpi::_sent_handles;
robin_hood::unordered_map<int, MsgTag> MyMpi::_tags;
bool MyMpi::_monitor_off;
int MyMpi::_monkey_flags = 0;

int handleId;

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
    if (finished) {
        if (!selfMessage) {
            int count = 0;
            MPICALL(MPI_Get_count(&status, MPI_BYTE, &count), "getcount" + std::to_string(id))
            if (count > 0 && count < (int)recvData.size()) {
                recvData.resize(count);
            }
        }
        if (tag == MSG_ANYTIME) {
            // Read msg tag of application layer and shrink data by its size
            memcpy(&tag, recvData.data()+recvData.size()-sizeof(int), sizeof(int));
            recvData.resize(recvData.size()-sizeof(int));
        }
    }

    return finished;
}

bool MessageHandle::shouldCancel(float elapsedTime) {
    // Non-finished, no self message, sufficiently old, not an anytime tag
    return !finished && !selfMessage && elapsedTime-creationTime > 60.f 
            && !MyMpi::isAnytimeTag(tag);
}

void MessageHandle::cancel() {
    MPICALL(MPI_Cancel(&request), "cancel" + std::to_string(id))
}


int MyMpi::nextHandleId() {
    return handleId++;
}

void MyMpi::init(int argc, char *argv[]) {

    int provided = -1;
    MPICALL(MPI_Init_thread(&argc, &argv, MPI_THREAD_SINGLE, &provided), std::string("init"))
    if (provided != MPI_THREAD_SINGLE) {
        std::cout << "ERROR initializing MPI: wanted id=" << MPI_THREAD_SINGLE 
                << ", got id=" << provided << std::endl;
        exit(1);
    }

    _max_msg_length = MyMpi::size(MPI_COMM_WORLD) * MAX_JOB_MESSAGE_PAYLOAD_PER_NODE + MAX_ANYTIME_MESSAGE_SIZE;
    handleId = 1;

    std::vector<MsgTag> tagList;
    /*                   Tag name                           anytime  */
    tagList.emplace_back(MSG_NOTIFY_JOB_ABORTING,           true); 
    tagList.emplace_back(MSG_ACCEPT_ADOPTION_OFFER,         true); 
    tagList.emplace_back(MSG_CONFIRM_JOB_REVISION_DETAILS,  true);
    tagList.emplace_back(MSG_REDUCE_DATA,                   true);
    tagList.emplace_back(MSG_BROADCAST_DATA,                true);
    tagList.emplace_back(MSG_COLLECTIVE_OPERATION,          false);
    tagList.emplace_back(MSG_CONFIRM_ADOPTION,              true); 
    tagList.emplace_back(MSG_CLIENT_FINISHED,               true);
    tagList.emplace_back(MSG_DO_EXIT,                       true);   
    tagList.emplace_back(MSG_REQUEST_NODE,                  true);
    tagList.emplace_back(MSG_REQUEST_NODE_ONESHOT,          true);
    tagList.emplace_back(MSG_SEND_CLIENT_RANK,              true);
    tagList.emplace_back(MSG_INCREMENTAL_JOB_FINISHED,      true);
    tagList.emplace_back(MSG_INTERRUPT,                     true); 
    tagList.emplace_back(MSG_SEND_APPLICATION_MESSAGE,      true); 
    tagList.emplace_back(MSG_NOTIFY_JOB_DONE,               true);
    tagList.emplace_back(MSG_NOTIFY_JOB_REVISION,           true); 
    tagList.emplace_back(MSG_OFFER_ADOPTION,                true); 
    tagList.emplace_back(MSG_REJECT_ONESHOT,                true); 
    tagList.emplace_back(MSG_QUERY_JOB_RESULT,              true); 
    tagList.emplace_back(MSG_QUERY_JOB_REVISION_DETAILS,    true); 
    tagList.emplace_back(MSG_QUERY_VOLUME,                  true); 
    tagList.emplace_back(MSG_REJECT_ADOPTION_OFFER,         true); 
    tagList.emplace_back(MSG_NOTIFY_RESULT_OBSOLETE,        true);
    tagList.emplace_back(MSG_SEND_JOB_DESCRIPTION,          false); 
    tagList.emplace_back(MSG_SEND_JOB_RESULT,               false); 
    tagList.emplace_back(MSG_SEND_JOB_REVISION_DATA,        false);
    tagList.emplace_back(MSG_SEND_JOB_REVISION_DETAILS,     true); 
    tagList.emplace_back(MSG_NOTIFY_JOB_TERMINATING,        true); 
    tagList.emplace_back(MSG_NOTIFY_VOLUME_UPDATE,          true); 
    tagList.emplace_back(MSG_WARMUP,                        true); 
    tagList.emplace_back(MSG_NOTIFY_RESULT_FOUND,           true);
    tagList.emplace_back(MSG_NOTIFY_NODE_LEAVING_JOB,       true); 
    
    for (const auto& tag : tagList) _tags[tag.id] = tag;
}

void MyMpi::setOptions(const Parameters& params) {
    _monitor_off = !params.isNotNull("mmpi");
    int verb = MyMpi::rank(MPI_COMM_WORLD) == 0 ? Console::INFO : Console::VVERB;
    if (params.isNotNull("delaymonkey")) {
        Console::log(verb, "Enabling delay monkey");
        _monkey_flags |= MONKEY_DELAY;
    }
    if (params.isNotNull("latencymonkey")) {
        Console::log(verb, "Enabling latency monkey");
        _monkey_flags |= MONKEY_LATENCY;
    }
}

void MyMpi::beginListening() {
    MyMpi::irecv(MPI_COMM_WORLD, MSG_ANYTIME);
    Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", _handles.back()->id, MSG_ANYTIME);
}

bool MyMpi::isAnytimeTag(int tag) {
    if (tag == MSG_ANYTIME) return true;
    assert(_tags.count(tag) || Console::fail("Unknown tag %i\n", tag));
    return _tags[tag].anytime;
}

void MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    isend(communicator, recvRank, tag, object.serialize());
}

void MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::vector<uint8_t>& object) {

    latencyMonkey();
    delayMonkey();

    bool selfMessage = rank(communicator) == recvRank;

    auto& handles = (selfMessage ? _handles : _sent_handles);
    handles.emplace_back(new MessageHandle(nextHandleId(), object));
    auto& handle = *handles.back();
    int appTag = tag;

    // Append a single zero to an otherwise empty message
    if (handle.sendData.empty()) handle.sendData.push_back(0);
    // Overwrite tag as MSG_ANYTIME, append application tag to message
    if (isAnytimeTag(tag)) {
        int prevSize = object.size();
        handle.sendData.resize(prevSize+sizeof(int));
        memcpy(handle.sendData.data()+prevSize, &tag, sizeof(int));
        tag = MSG_ANYTIME;
    }
    handle.tag = tag;

    Console::log(Console::VVVERB, "Msg ID=%i dest=%i tag=%i size=%i", handle.id, 
                recvRank, appTag, handle.sendData.size());
    
    if (selfMessage) {
        handle.recvData = handle.sendData;
        handle.source = recvRank;
        handle.selfMessage = true;
    } else {
        MPICALL(MPI_Isend(handle.sendData.data(), handle.sendData.size(), MPI_BYTE, recvRank, 
                tag, communicator, &handle.request), "isend"+std::to_string(handle.id))
    }
}

/*
MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    return send(communicator, recvRank, tag, object.serialize());
}

MessageHandlePtr MyMpi::send(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {
    MessageHandlePtr handle(new MessageHandle(nextHandleId(), object));
    if (rank(communicator) == recvRank) {
        handle.recvData = handle.sendData;
        handle.tag = tag;
        handle.source = recvRank;
        handle.selfMessage = true;
        _handles.insert(handle);
    } else {
        MPICALL(MPI_Send(handle.sendData->data(), handle.sendData->size(), MPI_BYTE, recvRank, tag, communicator), "send"+std::to_string(handle.id))
    }
    Console::log(Console::VVVERB, "Msg ID=%i sent", handle.id);
    return handle;
}
*/

void MyMpi::irecv(MPI_Comm communicator) {
    irecv(communicator, MPI_ANY_TAG);
}

void MyMpi::irecv(MPI_Comm communicator, int tag) {
    irecv(communicator, MPI_ANY_SOURCE, tag);
}

void MyMpi::irecv(MPI_Comm communicator, int source, int tag) {

    int msgSize = _max_msg_length;
    _handles.emplace_back(new MessageHandle(nextHandleId(), msgSize));
    auto& handle = *_handles.back();
    handle.tag = tag;

    MPICALL(MPI_Irecv(handle.recvData.data(), msgSize, MPI_BYTE, source, isAnytimeTag(tag) ? MSG_ANYTIME : tag, 
                communicator, &handle.request), "irecv"+std::to_string(handle.id))
}

void MyMpi::irecv(MPI_Comm communicator, int source, int tag, int size) {

    assert(source >= 0);
    _handles.emplace_back(new MessageHandle(nextHandleId(), size));
    auto& handle = *_handles.back();

    handle.source = source;
    handle.tag = tag;
    MPICALL(MPI_Irecv(handle.recvData.data(), size, MPI_BYTE, source, isAnytimeTag(tag) ? MSG_ANYTIME : tag, 
                communicator, &handle.request), "irecv"+std::to_string(handle.id))
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

std::optional<MessageHandle> MyMpi::poll(float elapsedTime) {

    std::optional<MessageHandle> foundHandle;

    // Find some ready handle
    size_t offset = 0;
    size_t size = _handles.size();
    for (size_t i = 0; i < size; i++) {
        auto& h = *_handles[i];
        if (!foundHandle && h.testReceived()) {
            foundHandle = std::move(h);
    
            if (!h.selfMessage && isAnytimeTag(foundHandle->tag)) {
                // Reset listener, appending a new handle to _handles
                MyMpi::irecv(MPI_COMM_WORLD, MSG_ANYTIME);
                // Move new handle from the back to the current position
                _handles[i] = std::move(_handles.back());
                _handles.resize(_handles.size()-1);
                Console::log(Console::VVVERB, "Msg ID=%i : listening to tag %i", _handles[i]->id, MSG_ANYTIME);
            } else {
                // Overwrite this position
                offset++;
            }

        } else if (h.shouldCancel(elapsedTime)) {
            // Cancel handle, mark this position to be overwritten
            h.cancel();
            offset++;

        } else if (offset > 0) {
            // Handle is not finished yet: Overwrite old handle
            _handles[i-offset] = std::move(_handles[i]);
        }
    }
    if (offset > 0) _handles.resize(size-offset);
    return foundHandle;
}

bool MyMpi::hasOpenSentHandles() {
    return !_sent_handles.empty();
}

void MyMpi::testSentHandles() {

    size_t offset = 0;
    size_t size = _sent_handles.size();
    
    for (size_t i = 0; i < size; i++) {
        auto& h = *_sent_handles[i];
        if (h.testSent()) {
            // Sending operation completed
            Console::log(Console::VVVERB, "Msg ID=%i isent", h.id);
            offset++;
        } else if (offset > 0) {
            _sent_handles[i-offset] = std::move(_sent_handles[i]);
        }
    }

    if (offset > 0) _sent_handles.resize(size-offset);
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

void MyMpi::latencyMonkey() {
    if (_monkey_flags & MONKEY_LATENCY) {
        float duration = 1 * 1000 + 9 * 1000 * Random::rand(); // Sleep between one and ten millisecs
        //Console::log(Console::VVVERB, "LATENCY_MONKEY %.3fs", 0.001 * 0.001 * duration);
        usleep(duration); 
    }
}

void MyMpi::delayMonkey() {
    if ((_monkey_flags & MONKEY_DELAY) && Random::rand() * 100 <= 1) { // chance of 1:100
        float duration = 1000 * 1000 * Random::rand(); // Sleep for up to one second
        Console::log(Console::VVVERB, "DELAY_MONKEY %.3fs", 0.001 * 0.001 * duration);
        usleep(duration); 
    }
}

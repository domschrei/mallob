
#include <chrono>
#include <unistd.h>
#include <ctime>
#include <algorithm>

#include "mympi.hpp"

#include "util/random.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "comm/mpi_monitor.hpp"

#define MPICALL(cmd, str) {if (!MyMpi::_monitor_off) {initcall((str).c_str());} \
int err = cmd; if (!MyMpi::_monitor_off) endcall(); chkerr(err);}

int MyMpi::_max_msg_length;
std::vector<MessageHandlePtr> MyMpi::_handles;
std::vector<MessageHandlePtr> MyMpi::_sent_handles;
robin_hood::unordered_map<int, MsgTag> MyMpi::_tags;
bool MyMpi::_monitor_off;
int MyMpi::_monkey_flags = 0;

int handleId;

void chkerr(int err) {
    if (err != 0) {
        log(V0_CRIT, "MPI ERROR errcode=%i\n", err);
        Logger::getMainInstance().flush();
        abort();
    }
}

bool MessageHandle::testSent() {
    assert(!selfMessage || log_return_false("Attempting to MPI_Test a self message!\n"));
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
            assert(status.MPI_SOURCE >= 0 || log_return_false("MPI_SOURCE = %i\n", status.MPI_SOURCE));
            source = status.MPI_SOURCE;
        }
    }

    // Resize received data vector to actual received size
    if (finished) {
        if (!selfMessage) {
            int count = 0;
            MPICALL(MPI_Get_count(&status, MPI_BYTE, &count), "getcount" + std::to_string(id))
            if (count > 0 && count != MPI_UNDEFINED && count < (int)recvData.size()) {
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
    int verb = MyMpi::rank(MPI_COMM_WORLD) == 0 ? V2_INFO : V4_VVER;
    if (params.isNotNull("delaymonkey")) {
        log(verb, "Enabling delay monkey\n");
        _monkey_flags |= MONKEY_DELAY;
    }
    if (params.isNotNull("latencymonkey")) {
        log(verb, "Enabling latency monkey\n");
        _monkey_flags |= MONKEY_LATENCY;
    }
    _max_msg_length = sizeof(int) * MyMpi::size(MPI_COMM_WORLD) * params.getIntParam("cbbs") + 1024;
}

void MyMpi::beginListening() {
    MyMpi::irecv(MPI_COMM_WORLD, MSG_ANYTIME);
    log(V5_DEBG, "Msg ID=%i : listening to tag %i\n", _handles.back()->id, MSG_ANYTIME);
}

bool MyMpi::isAnytimeTag(int tag) {
    if (tag == MSG_ANYTIME) return true;
    assert(_tags.count(tag) || log_return_false("Unknown tag %i\n", tag));
    return _tags[tag].anytime;
}

void MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object) {
    isend(communicator, recvRank, tag, object.serialize());
}

void MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::vector<uint8_t>& object) {

    bool selfMessage = rank(communicator) == recvRank;
    auto& handles = (selfMessage ? _handles : _sent_handles);

    // Append a single zero to an otherwise empty message
    handles.emplace_back(new MessageHandle(nextHandleId(), 
            object.empty() ? std::vector<uint8_t>(1, 0) : object
    ));
    
    doIsend(communicator, recvRank, tag);
}

void MyMpi::isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object) {

    bool selfMessage = rank(communicator) == recvRank;
    auto& handles = (selfMessage ? _handles : _sent_handles);
    
    // Append a single zero to an otherwise empty message
    if (object->empty()) {
        handles.emplace_back(new MessageHandle(nextHandleId(), std::vector<uint8_t>(1, 0)));
    } else {
        handles.emplace_back(new MessageHandle(nextHandleId(), object));
    }

    doIsend(communicator, recvRank, tag);
}

void MyMpi::doIsend(MPI_Comm communicator, int recvRank, int tag) {

    latencyMonkey();
    delayMonkey();

    bool selfMessage = rank(communicator) == recvRank;
    auto& handles = (selfMessage ? _handles : _sent_handles);

    auto& handle = *handles.back();
    int appTag = tag;

    // Overwrite tag as MSG_ANYTIME, append application tag to message
    if (isAnytimeTag(tag)) {
        handle.appendTagToSendData(tag);
        tag = MSG_ANYTIME;
        assert(handle.getSendData().size() <= _max_msg_length 
            || log_return_false("Too long message of size %i\n", _max_msg_length));
    }
    handle.tag = tag;

    log(V5_DEBG, "Msg ID=%i dest=%i tag=%i size=%i\n", handle.id, 
                recvRank, appTag, handle.getSendData().size());
    
    if (selfMessage) {
        handle.receiveSelfMessage(handle.getSendData(), recvRank);
    } else {
        MPICALL(MPI_Isend(handle.getSendData().data(), handle.getSendData().size(), MPI_BYTE, recvRank, 
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
        handle.getRecvData() = handle.getSendData();
        handle.tag = tag;
        handle.source = recvRank;
        handle.selfMessage = true;
        _handles.insert(handle);
    } else {
        MPICALL(MPI_Send(handle.getSendData()->data(), handle.getSendData()->size(), MPI_BYTE, recvRank, tag, communicator), "send"+std::to_string(handle.id))
    }
    log(V5_DEBG, "Msg ID=%i sent\n", handle.id);
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

MessageHandlePtr MyMpi::poll(float elapsedTime) {

    MessageHandlePtr foundHandle;

    // Traverse all active handles
    for (size_t i = 0; i < _handles.size(); i++) {
        auto& h = _handles[i];
        bool handleRemoved = false;

        if (!foundHandle && h->testReceived()) {    
            // Finished handle found!
            foundHandle = std::move(h);
            handleRemoved = true;

            // Any listener to reset?
            if (!foundHandle->selfMessage && isAnytimeTag(foundHandle->tag)) {
                // Reset listener, appending a new handle to _handles
                MyMpi::irecv(MPI_COMM_WORLD, MSG_ANYTIME);
            }

        } else if (h->shouldCancel(elapsedTime)) {
            // Cancel handle, mark this position to be overwritten
            h->cancel();
            handleRemoved = true;
        }

        // Shorten the list of handles if one was removed
        if (handleRemoved) {
            if (i+1 < _handles.size()) {
                // Move new handle from the back to the current position
                _handles[i] = std::move(_handles.back());
            }
            _handles.resize(_handles.size()-1);
        }
    }

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
            log(V5_DEBG, "Msg ID=%i isent\n", h.id);
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
        //log(V5_DEBG, "LATENCY_MONKEY %.3fs\n", 0.001 * 0.001 * duration);
        usleep(duration); 
    }
}

void MyMpi::delayMonkey() {
    if ((_monkey_flags & MONKEY_DELAY) && Random::rand() * 100 <= 1) { // chance of 1:100
        float duration = 1000 * 1000 * Random::rand(); // Sleep for up to one second
        log(V5_DEBG, "DELAY_MONKEY %.3fs\n", 0.001 * 0.001 * duration);
        usleep(duration); 
    }
}

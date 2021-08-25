
#include "comm/message_queue.hpp"

#include "comm/message_handle.hpp"

#include <list>
#include <cmath>
#include "util/assert.hpp"
#include <unistd.h>

#include "util/hashing.hpp"
#include "util/sys/background_worker.hpp"
#include "util/logger.hpp"
#include "comm/msgtags.h"
#include "util/ringbuffer.hpp"

MessageQueue::MessageQueue(int maxMsgSize) : _max_msg_size(maxMsgSize), 
    _fragmented_queue(128), _fused_queue(128), _garbage_queue(128) {
    
    MPI_Comm_rank(MPI_COMM_WORLD, &_my_rank);
    _recv_data = (uint8_t*) malloc(maxMsgSize+20);

    MPI_Irecv(_recv_data, maxMsgSize+20, MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &_recv_request);

    _batch_assembler.run([&]() {runFragmentedMessageAssembler();});
    _gc.run([&]() {runGarbageCollector();});
}

MessageQueue::~MessageQueue() {
    _batch_assembler.stop();
    _gc.stop();
    free(_recv_data);
}

void MessageQueue::registerCallback(int tag, const MsgCallback& cb) {
    _callbacks[tag] = cb;
}

void MessageQueue::registerSentCallback(std::function<void(int)> callback) {
    _send_done_callback = callback;
}

int MessageQueue::send(DataPtr data, int dest, int tag) {

    // Initialize send handle
    sendQueue.push_back(SendHandle());
    SendHandle& h = sendQueue.back();
    h.id = runningSendId++;
    h.data = data;
    h.dest = dest;
    h.tag = tag;
    int msgSize = h.data->size();

    if (dest == _my_rank) {
        // Self message
        _self_recv_queue.push_back(std::move(h));
        sendQueue.pop_back();
        return _self_recv_queue.back().id;
    }

    if (data->size() > _max_msg_size+3*sizeof(int)) {
        log(V4_VVER, "MQ init batch send\n");
        // Batch data, only send first batch
        h.sentBatches = 0;
        h.totalNumBatches = getTotalNumBatches(h);
        int sendTag = prepareSendHandleForNextBatch(h);
        MPI_Isend(h.tempStorage.data(), h.tempStorage.size(), MPI_BYTE, dest, 
            sendTag, MPI_COMM_WORLD, &h.request);
        log(V4_VVER, "MQ sent 1st batch\n");
    } else {
        // Directly send entire message
        MPI_Isend(h.data->data(), msgSize, MPI_BYTE, dest, tag, MPI_COMM_WORLD, &h.request);
    }

    return h.id;
}

void MessageQueue::advance() {
    //log(V5_DEBG, "BEGADV\n");
    _iteration++;
    processReceived();
    processSelfReceived();
    processAssembledReceived();
    processSent();
    //log(V5_DEBG, "ENDADV\n");
}

void MessageQueue::runFragmentedMessageAssembler() {

    while (_batch_assembler.continueRunning()) {

        usleep(1000); // 1 ms
        if (_fragmented_queue.empty()) continue;

        auto opt = _fragmented_queue.consume();
        if (!opt.has_value()) continue;
        ReceiveFragment& data = opt.value();

        if (data.dataFragments.empty()) continue;

        // Assemble fragments
        MessageHandle h;
        h.source = data.source;
        h.tag = data.tag;
        size_t sumOfSizes = 0;
        for (size_t i = 0; i < data.dataFragments.size(); i++) {
            const auto& frag = data.dataFragments[i];
            assert(frag || log_return_false("No valid fragment %i found!\n", i));
            sumOfSizes += frag->size();
        }
        std::vector<uint8_t> outData(sumOfSizes);
        size_t offset = 0;
        for (const auto& frag : data.dataFragments) {
            memcpy(outData.data()+offset, frag->data(), frag->size());
            offset += frag->size();
        }
        h.setReceive(std::move(outData));
        // Put into finished queue
        while (!_fused_queue.produce(std::move(h))) {}
    }
}

void MessageQueue::runGarbageCollector() {

    while (_gc.continueRunning()) {
        usleep(1000*1000); // 1s            
        if (_garbage_queue.empty()) continue;
        auto opt = _garbage_queue.consume();
        if (!opt.has_value()) continue;
        auto& dataPtr = opt.value();
        dataPtr.reset();
    }
}

void MessageQueue::processReceived() {
    // Test receive
    //log(V5_DEBG, "MQ TEST\n");
    int flag;
    MPI_Status status;
    MPI_Test(&_recv_request, &flag, &status);
    if (!flag) return;

    // Message finished
    int msglen;
    MPI_Get_count(&status, MPI_BYTE, &msglen);
    const int source = status.MPI_SOURCE;
    int tag = status.MPI_TAG;
    log(V5_DEBG, "MQ RECV n=%i s=[%i] t=%i\n", msglen, source, tag);

    if (tag >= MSG_OFFSET_BATCHED_METADATA_AT_FRONT) {
        // Fragment of a message

        // To which end has the metadata been appended?
        bool appendedAtBack = false;
        if (tag >= MSG_OFFSET_BATCHED_METADATA_AT_BACK) {
            appendedAtBack = true;
            tag -= MSG_OFFSET_BATCHED_METADATA_AT_BACK;
        } else tag -= MSG_OFFSET_BATCHED_METADATA_AT_FRONT;

        int offset = 0;
        int id, sentBatch, totalNumBatches;
        if (appendedAtBack) {
            // Read meta data from end of message
            memcpy(&id, _recv_data+msglen - 3*sizeof(int), sizeof(int));
            memcpy(&sentBatch, _recv_data+msglen - 2*sizeof(int), sizeof(int));
            memcpy(&totalNumBatches, _recv_data+msglen - sizeof(int), sizeof(int));
        } else {
            // Read meta data from front of message
            memcpy(&id, _recv_data, sizeof(int));
            memcpy(&sentBatch, _recv_data+sizeof(int), sizeof(int));
            memcpy(&totalNumBatches, _recv_data+2*sizeof(int), sizeof(int));
            offset = 3*sizeof(int);
        }
        msglen -= 3*sizeof(int);
        
        // Store data in fragments structure
        //log(V5_DEBG, "MQ STORE\n");
        auto& fragment = _fragmented_messages[std::pair<int, int>(source, id)];
        fragment.source = source;
        fragment.tag = tag;
        if (sentBatch >= fragment.dataFragments.size()) fragment.dataFragments.resize(sentBatch+1);
        //log(V5_DEBG, "MQ STORE alloc\n");
        fragment.dataFragments[sentBatch].reset(new std::vector<uint8_t>(_recv_data+offset, _recv_data+offset+msglen));
        fragment.receivedFragments++;
        //log(V5_DEBG, "MQ STORE produce\n");
        // All fragments of the message received?
        if (fragment.receivedFragments == totalNumBatches) {
            while (!_fragmented_queue.produce(std::move(fragment))) {}
        }
    } else {
        // Single message
        //log(V5_DEBG, "MQ singlerecv\n");
        MessageHandle h;
        h.setReceive(std::vector<uint8_t>(_recv_data, _recv_data+msglen));
        h.tag = tag;
        h.source = source;
        //log(V5_DEBG, "MQ cb\n");
        _callbacks.at(h.tag)(h);
        //log(V5_DEBG, "MQ dealloc\n");
    }
    // Reset recv handle
    //log(V5_DEBG, "MQ MPI_Irecv\n");
    MPI_Irecv(_recv_data, _max_msg_size+20, MPI_BYTE, MPI_ANY_SOURCE, 
        MPI_ANY_TAG, MPI_COMM_WORLD, &_recv_request);
}

void MessageQueue::processSelfReceived() {
    if (_self_recv_queue.empty()) return;
    // move queue due to concurrent modification in callback
    std::vector<SendHandle> copiedQueue(std::move(_self_recv_queue)); 
    _self_recv_queue.clear();
    for (auto& sh : copiedQueue) {
        MessageHandle h;
        h.tag = sh.tag;
        h.source = sh.dest;
        h.setReceive(std::move(*sh.data));
        _callbacks.at(h.tag)(h);
        _send_done_callback(sh.id); // notify completion
    }
}

void MessageQueue::processAssembledReceived() {
    // Process fully assembled batched messages
    auto opt = _fused_queue.consume();
    while (opt.has_value()) {
        auto& h = opt.value();

        log(V5_DEBG, "tag: %i\n", h.tag);
        _callbacks.at(h.tag)(h);
        
        if (h.getRecvData().size() > _max_msg_size) {
            // Concurrent deallocation of large chunk of data
            while (!_garbage_queue.produce(
                DataPtr(
                    new std::vector<uint8_t>(
                        h.moveRecvData()
                    )
                )
            )) {}
        }

        opt = _fused_queue.consume();
    }
}

void MessageQueue::processSent() {
    // Test each send handle
    int numTested = 0;
    for (auto it = sendQueue.begin(); it != sendQueue.end() && numTested < 6; ) {
        auto& h = *it;
        int flag;
        MPI_Status status;
        MPI_Test(&h.request, &flag, &status);
        numTested++;

        if (!flag) {
            it++; // go to next handle
            continue;
        }
        
        // Sent!
        //log(V5_DEBG, "MQ SENT n=%i d=[%i] t=%i\n", h.data->size(), h.dest, h.tag);
        
        // Batched?
        if (isBatched(h)) {
            h.sentBatches++;
            if (!isFinished(h)) {
                // Send next batch
                int sendTag = prepareSendHandleForNextBatch(h);
                MPI_Isend(h.tempStorage.data(), h.tempStorage.size(), MPI_BYTE, h.dest, 
                    sendTag, MPI_COMM_WORLD, &h.request);
                continue; // test this handle another time
            }
        }

        // Notify completion
        _send_done_callback(h.id); 

        if (h.data->size() > _max_msg_size) {
            // Concurrent deallocation of SendHandle's large chunk of data
            while (!_garbage_queue.produce(std::move(h.data))) {}
        }
        
        // Remove handle
        it = sendQueue.erase(it);
    }
}

int MessageQueue::prepareSendHandleForNextBatch(SendHandle& h) {
    int totalNumBatches = h.totalNumBatches;
    size_t begin = h.sentBatches*_max_msg_size;
    size_t end = std::min(h.data->size(), (h.sentBatches+1)*_max_msg_size);
    h.tempStorage.resize((end-begin)+3*sizeof(int));

    // Copy actual data
    memcpy(h.tempStorage.data(), h.data->data()+begin, end-begin);
    // Copy meta data at insertion point
    memcpy(h.tempStorage.data()+(end-begin), &h.id, sizeof(int));
    memcpy(h.tempStorage.data()+(end-begin)+sizeof(int), &h.sentBatches, sizeof(int));
    memcpy(h.tempStorage.data()+(end-begin)+2*sizeof(int), &totalNumBatches, sizeof(int));

    return h.tag + MSG_OFFSET_BATCHED_METADATA_AT_BACK;
}
size_t MessageQueue::getTotalNumBatches(const SendHandle& h) const {return std::ceil(h.data->size() / (float)_max_msg_size);}
bool MessageQueue::isBatched(const SendHandle& h) const {return h.sentBatches >= 0;}
bool MessageQueue::isFinished(const SendHandle& h) const {return h.sentBatches == h.totalNumBatches;}

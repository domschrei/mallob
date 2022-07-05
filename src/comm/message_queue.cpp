
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

MessageQueue::MessageQueue(int maxMsgSize) : _max_msg_size(maxMsgSize) {
    
    MPI_Comm_rank(MPI_COMM_WORLD, &_my_rank);
    _recv_data = (uint8_t*) malloc(maxMsgSize+20);

    _current_recv_tag = &_default_tag_var;
    _current_send_tag = &_default_tag_var;

    resetReceiveHandle();

    _batch_assembler.run([&]() {
        Proc::nameThisThread("MsgAssembler");
        runFragmentedMessageAssembler();
    });
    _gc.run([&]() {
        Proc::nameThisThread("MsgGarbColl");
        runGarbageCollector();
    });
}

MessageQueue::~MessageQueue() {
    _batch_assembler.stop();
    _gc.stop();
    free(_recv_data);
}

void MessageQueue::registerCallback(int tag, const MsgCallback& cb) {
    if (_callbacks.count(tag)) {
        LOG(V0_CRIT, "More than one callback for tag %i!\n", tag);
        abort();
    }
    _callbacks[tag] = cb;
}

void MessageQueue::registerSentCallback(int tag, const SendDoneCallback& cb) {
    if (_send_done_callbacks.count(tag)) {
        LOG(V0_CRIT, "More than one callback for tag %i!\n", tag);
        abort();
    }
    _send_done_callbacks[tag] = cb;
}

void MessageQueue::clearCallbacks() {
    _callbacks.clear();
    _send_done_callbacks.clear();
}

int MessageQueue::send(DataPtr data, int dest, int tag) {

    *_current_send_tag = tag;

    // Initialize send handle
    {
        SendHandle handle(_running_send_id++, dest, tag, data, _max_msg_size);

        int msglen = handle.data->size();
        LOG(V5_DEBG, "MQ SEND n=%i d=[%i] t=%i c=(%i,...,%i,%i,%i)\n", handle.data->size(), dest, tag, 
            msglen>=1*sizeof(int) ? *(int*)(handle.data->data()) : 0, 
            msglen>=3*sizeof(int) ? *(int*)(handle.data->data()+msglen - 3*sizeof(int)) : 0, 
            msglen>=2*sizeof(int) ? *(int*)(handle.data->data()+msglen - 2*sizeof(int)) : 0, 
            msglen>=1*sizeof(int) ? *(int*)(handle.data->data()+msglen - 1*sizeof(int)) : 0);

        if (dest == _my_rank) {
            // Self message
            _self_recv_queue.push_back(std::move(handle));
            return _self_recv_queue.back().id;
        }

        _send_queue.push_back(std::move(handle));
    }

    SendHandle& h = _send_queue.back();
    if (_num_concurrent_sends < _max_concurrent_sends) {
        h.sendNext();
        _num_concurrent_sends++;
    }

    *_current_send_tag = 0;
    return h.id;
}

void MessageQueue::cancelSend(int sendId) {

    for (auto& h : _send_queue) {
        if (h.id != sendId) continue;

        // Found fitting handle
        h.cancel();
        break;
    }
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

        _fragmented_cond_var.wait(_fragmented_mutex, [&]() {return !_fragmented_queue.empty();});

        ReceiveFragment data;
        {
            if (!_fragmented_mutex.tryLock()) continue;
            if (_fragmented_queue.empty()) {
                _fragmented_mutex.unlock();
                continue;
            }
            data = std::move(_fragmented_queue.front());
            _fragmented_queue.pop_front();
            _fragmented_mutex.unlock();
        }
        if (data.dataFragments.empty()) continue;

        if (data.isCancelled()) {
            // Receive message was cancelled in between batches: 
            // concurrently clean up any fragments already received
            auto lock = _garbage_mutex.getLock();
            int numFragments = 0;
            for (auto& frag : data.dataFragments) if (frag != nullptr) {
                std::vector<uint8_t>* ptr = frag.release();
                _garbage_queue.push_back(DataPtr(ptr));
                atomics::incrementRelaxed(_num_garbage);
                numFragments++;
            }
            LOG(V4_VVER, "MSG id=%i cancelled (%i fragments)\n", data.id, numFragments);
            continue;
        }

        // Assemble fragments
        MessageHandle h;
        h.source = data.source;
        h.tag = data.tag;
        size_t sumOfSizes = 0;
        for (size_t i = 0; i < data.dataFragments.size(); i++) {
            const auto& frag = data.dataFragments[i];
            assert(frag || LOG_RETURN_FALSE("No valid fragment %i found!\n", i));
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
        {
            auto lock = _fused_mutex.getLock();
            _fused_queue.push_back(std::move(h));
            atomics::incrementRelaxed(_num_fused);
        }
    }
}

void MessageQueue::runGarbageCollector() {

    while (_gc.continueRunning()) {
        usleep(1000*1000); // 1s
        while (_num_garbage > 0) {
            DataPtr dataPtr;
            {
                auto lock = _garbage_mutex.getLock();
                dataPtr = std::move(_garbage_queue.front());
                _garbage_queue.pop_front();
            }
            //if (dataPtr.use_count() > 0) {
            //    LOG(V4_VVER, "GC %p : use count %i\n", dataPtr.get(), dataPtr.use_count());
            //}
            dataPtr.reset();
            atomics::decrementRelaxed(_num_garbage);
        }
    }
}

void MessageQueue::processReceived() {

    int k = 0;
    while (k < _num_receives_per_loop) {
        k++;

        // Test receive
        //log(V5_DEBG, "MQ TEST\n");
        int flag = false;
        MPI_Status status;
        MPI_Test(&_recv_request, &flag, &status);
        if (!flag) {
            // Handle is not finished:
            // reset #receives per loop
            _num_receives_per_loop = _base_num_receives_per_loop;
            return;
        }

        // Message finished
        const int source = status.MPI_SOURCE;
        int tag = status.MPI_TAG;
        int msglen;
        MPI_Get_count(&status, MPI_BYTE, &msglen);
        LOG(V5_DEBG, "MQ RECV n=%i s=[%i] t=%i c=(%i,...,%i,%i,%i)\n", msglen, source, tag, 
                msglen>=1*sizeof(int) ? *(int*)(_recv_data) : 0, 
                msglen>=3*sizeof(int) ? *(int*)(_recv_data+msglen - 3*sizeof(int)) : 0, 
                msglen>=2*sizeof(int) ? *(int*)(_recv_data+msglen - 2*sizeof(int)) : 0, 
                msglen>=1*sizeof(int) ? *(int*)(_recv_data+msglen - 1*sizeof(int)) : 0);

        if (tag >= MSG_OFFSET_BATCHED) {
            // Fragment of a message

            tag -= MSG_OFFSET_BATCHED;
            int id = ReceiveFragment::readId(_recv_data, msglen);
            auto key = std::pair<int, int>(source, id);
            
            if (!_fragmented_messages.count(key)) {
                _fragmented_messages.emplace(key, ReceiveFragment(source, id, tag));
            }
            auto& fragment = _fragmented_messages[key];

            fragment.receiveNext(source, tag, _recv_data, msglen);

            resetReceiveHandle();

            if (fragment.isCancelled() || fragment.isFinished()) {
                {
                    auto lock = _fragmented_mutex.getLock();
                    _fragmented_queue.push_back(std::move(fragment));
                    _fragmented_messages.erase(key);
                }
                _fragmented_cond_var.notify();
            }

            // Receive next message
            continue;
        }

        // Single message
        //log(V5_DEBG, "MQ singlerecv\n");
        MessageHandle h;
        h.setReceive(std::vector<uint8_t>(_recv_data, _recv_data+msglen));
        h.tag = tag;
        h.source = source;

        resetReceiveHandle();

        // Process message according to its tag-specific callback
        *_current_recv_tag = h.tag;
        _callbacks.at(h.tag)(h);
        *_current_recv_tag = 0;
    }

    // Increase #receives per loop for the next time, if necessary
    if (k == _num_receives_per_loop && _num_receives_per_loop < 1000) {
        _num_receives_per_loop *= 2;
    }
}

void MessageQueue::resetReceiveHandle() {
    // Reset recv handle
    //log(V5_DEBG, "MQ MPI_Irecv\n");
    MPI_Irecv(_recv_data, _max_msg_size+20, MPI_BYTE, MPI_ANY_SOURCE, 
        MPI_ANY_TAG, MPI_COMM_WORLD, &_recv_request);
}

void MessageQueue::signalCompletion(int tag, int id) {
    auto it = _send_done_callbacks.find(tag);
    if (it != _send_done_callbacks.end()) {
        auto& callback = it->second;
        callback(id);
    }
}

void MessageQueue::processSelfReceived() {
    if (_self_recv_queue.empty()) return;
    // copy content of queue due to concurrent modification in callback
    // (up to x elements in order to stay responsive)
    std::vector<SendHandle> copiedQueue;
    while (!_self_recv_queue.empty() && copiedQueue.size() < 4) {
        copiedQueue.push_back(std::move(_self_recv_queue.front()));
        _self_recv_queue.pop_front();
    }
    for (auto& sh : copiedQueue) {
        MessageHandle h;
        h.tag = sh.tag;
        h.source = sh.dest;
        h.setReceive(std::move(*sh.data));
        *_current_recv_tag = h.tag;
        _callbacks.at(h.tag)(h);
        signalCompletion(h.tag, sh.id);
        *_current_recv_tag = 0;
    }
}

void MessageQueue::processAssembledReceived() {

    int numFused = _num_fused.load(std::memory_order_relaxed);
    if (numFused > 0 && _fused_mutex.tryLock()) {

        int consumed = 0;
        while (!_fused_queue.empty() && consumed < 4) {

            auto& h = _fused_queue.front();
            LOG(V5_DEBG, "MQ FUSED t=%i\n", h.tag);
            
            *_current_recv_tag = h.tag;
            _callbacks.at(h.tag)(h);
            *_current_recv_tag = 0;
            
            if (h.getRecvData().size() > _max_msg_size) {
                // Concurrent deallocation of large chunk of data
                auto lock = _garbage_mutex.getLock();
                _garbage_queue.push_back(
                    DataPtr(
                        new std::vector<uint8_t>(
                            h.moveRecvData()
                        )
                    )
                );
                atomics::incrementRelaxed(_num_garbage);
            }
            _fused_queue.pop_front();
            atomics::decrementRelaxed(_num_fused);

            consumed++;
            if (consumed >= 4) break;
        }

        _fused_mutex.unlock();
    }
}

void MessageQueue::processSent() {

    auto it = _send_queue.begin();
    bool uninitiatedHandlesPresent = false;

    // Test each send handle
    while (it != _send_queue.end()) {
        
        SendHandle& h = *it;

        if (!h.isInitiated()) {
            // Message has not been sent yet
            uninitiatedHandlesPresent = true;
            ++it; // go to next handle
            continue;
        }

        if (!h.test()) {
            ++it; // go to next handle
            continue;
        }
        
        // Sent!
        //log(V5_DEBG, "MQ SENT n=%i d=[%i] t=%i\n", h.data->size(), h.dest, h.tag);
        bool completed = true;

        // Batched?
        if (h.isBatched()) {
            // Batch of a large message sent
            LOG(V5_DEBG, "MQ SENT id=%i %i/%i n=%i d=[%i] t=%i c=(%i,...,%i,%i,%i)\n", h.id, h.sentBatches, 
                h.totalNumBatches, h.data->size(), h.dest, h.tag, 
                *(int*)(h.tempStorage.data()), 
                *(int*)(h.tempStorage.data()+h.tempStorage.size()-3*sizeof(int)), 
                *(int*)(h.tempStorage.data()+h.tempStorage.size()-2*sizeof(int)),
                *(int*)(h.tempStorage.data()+h.tempStorage.size()-1*sizeof(int)));

            // More batches yet to send?
            if (!h.isFinished()) {
                // Send next batch
                h.sendNext();
                completed = false;
            }
        }

        if (completed) {
            // Notify completion
            signalCompletion(h.tag, h.id);
            _num_concurrent_sends--;

            if (h.data->size() > _max_msg_size) {
                // Concurrent deallocation of SendHandle's large chunk of data
                auto lock = _garbage_mutex.getLock();
                _garbage_queue.push_back(std::move(h.data));
                atomics::incrementRelaxed(_num_garbage);
            } else {
                // Direct deallocation of SendHandle's data
                h.data.reset();
            }
            
            // Remove handle
            it = _send_queue.erase(it); // go to next handle
        } else {
            ++it; // go to next handle
        }
    }

    if (!uninitiatedHandlesPresent) return;

    // Initiate sending messages which have not been initiated yet
    // as long as there is a "send slot" available to do so
    it = _send_queue.begin();
    while (_num_concurrent_sends < _max_concurrent_sends && it != _send_queue.end()) {
        SendHandle& h = *it;
        if (!h.isInitiated()) {
            h.sendNext();
            _num_concurrent_sends++;
        }
        ++it;
    }
}

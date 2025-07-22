
#include "message_queue.hpp"

#include <stdlib.h>                             // for free, malloc, abort
#include <string.h>                             // for memcpy
#include <unistd.h>                             // for size_t, usleep
#include <assert.h>
#include <cstdint>                              // for uint8_t
#include <list>                                 // for list, _List_iterator
#include <memory>                               // for unique_ptr, __shared_...
#include <vector>                               // for vector

#include "comm/msg_queue/message_handle.hpp"    // for MessageHandle
#include "comm/msg_queue/receive_fragment.hpp"  // for ReceiveFragment
#include "comm/msg_queue/send_handle.hpp"       // for SendHandle, DataPtr
#include "comm/msgtags.h"                       // for MSG_OFFSET_BATCHED
#include "comm/mympi.hpp"
#include "util/logger.hpp"                      // for LOG, LOGGER_LOG_V5
#include "util/sys/atomics.hpp"                 // for incrementRelaxed, dec...
#include "util/sys/background_worker.hpp"       // for BackgroundWorker
#include "util/sys/proc.hpp"                    // for Proc


MessageQueue::MessageQueue(int maxMsgSize) : _max_msg_size(maxMsgSize) {
    
    MPI_Comm_rank(MPI_COMM_WORLD, &_my_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &_comm_size);
    _recv_data_1 = (uint8_t*) malloc(maxMsgSize+20);
    _recv_data_2 = (uint8_t*) malloc(maxMsgSize+20);

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

void MessageQueue::close() {

    // Cancel batched send messages
    for (auto& h : _send_queue) h.cancel();
    // Advance until all outgoing messages have been processed
    while (hasOpenSends()) advance();
    // Make sure that all sent handles are also processed at the receiver side
    for (int i = 0; i < 1 + (MyMpi::size(MPI_COMM_WORLD) * _max_concurrent_sends) / _base_num_receives_per_loop; i++)
        advance();

    // Notify background threads to stop and wake them up
    _batch_assembler.stopWithoutWaiting();
    {
        auto lock = _fragmented_mutex.getLock();
        _fragmented_queue.push_back(ReceiveFragment());
    }
    _fragmented_cond_var.notify();
    _gc.stopWithoutWaiting();
    {
        auto lock = _garbage_mutex.getLock();
        _garbage_queue.push_back(DataPtr());
    }
    _garbage_cond_var.notify();

    // Join background threads
    _batch_assembler.stop();
    _gc.stop();
}

MessageQueue::~MessageQueue() {
    // Cancel final open receive request
    if (_recv_request != MPI_REQUEST_NULL) {
        MPI_Cancel(&_recv_request);
        MPI_Request_free(&_recv_request);
    }
    // Free receive buffers
    free(_recv_data_1);
    free(_recv_data_2);
}

MessageQueue::CallbackRef MessageQueue::registerCallback(int tag, const MsgCallback& cb) {
    if (_callbacks.count(tag)) {
        LOG(V1_WARN, "[WARN] More than one callback for tag %i!\n", tag);
    }
    _callbacks[tag].push_back(cb);
    auto it = _callbacks[tag].end();
    --it;
    return it;
}

void MessageQueue::initializeConditionalCallbacks(int tag) {
    // "macro" callback
    registerCallback(tag, [this](MessageHandle& h) {
        assert(!h.getRecvData().empty());
        bool returnedToSender = h.getRecvData().back() != 0;
        bool accepted {false};
        std::vector<ConditionalMsgCallback> callbacks(_cond_callbacks.at(h.tag).begin(),
            _cond_callbacks.at(h.tag).end());
        for (auto& f : callbacks) {
            LOG(V5_DEBG, "[msgq] try cb for tag=%i\n", h.tag);
            MessageHandle copy(h);
            if (f(copy)) accepted = true;
        }
        if (!accepted && !returnedToSender) {
            LOG_ADD_DEST(V5_DEBG, "[msqq] tag %i : return to sender", h.source, h.tag);
            h.getRecvData().back() = 1; // returning-to-sender bit
            MyMpi::isend(h.source, h.tag, h.moveRecvData());
        }
    });
    _cond_callbacks[tag]; // initialize empty list of conditional callbacks
}

MessageQueue::CondCallbackRef MessageQueue::registerConditionalCallback(int tag, const ConditionalMsgCallback& cb) {
    assert(_cond_callbacks.count(tag) || log_return_false("[ERROR] Conditional callbacks not initialized for tag %i"
        " - did you call initializeConditionalCallbacks(tag) before?\n", tag));
    _cond_callbacks.at(tag).push_back(cb);
    auto it = _cond_callbacks[tag].end();
    --it;
    return it;
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

void MessageQueue::clearCallback(int tag, const CallbackRef& ref) {
    _callbacks[tag].erase(ref);
}
void MessageQueue::clearConditionalCallback(int tag, const CondCallbackRef& ref) {
    _cond_callbacks[tag].erase(ref);
}

int MessageQueue::send(const DataPtr& data, int dest, int tag, bool fromMainThread) {

    if (!fromMainThread) {
        auto lock = _mtx_out_msgs.getLock();
        _out_msgs.push_back({data, dest, tag});
        _check_out_msgs = true;
        return 0;
    }

    *_current_send_tag = tag;

    // Initialize send handle
    if (dest == _my_rank) {
        // Self message
        _self_recv_queue.emplace_back(_running_send_id++, dest, tag, data, _max_msg_size);
        SendHandle& h = _self_recv_queue.back();
        h.printSendMsg();
        return h.id;
    }

    _send_queue.emplace_back(_running_send_id++, dest, tag, data, _max_msg_size);
    SendHandle& h = _send_queue.back();
    h.printSendMsg();
    if (_num_concurrent_sends < _max_concurrent_sends) {
        h.sendNext(_max_msg_size);
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

    // Prepare sending outgoing messages from other (non main) threads
    if (_check_out_msgs && _mtx_out_msgs.tryLock()) {
        while (!_out_msgs.empty()) {
            auto outMsg = _out_msgs.front(); _out_msgs.pop_front();
            send(outMsg.data, outMsg.dest, outMsg.tag);
        }
        _mtx_out_msgs.unlock();
    }

    _iteration++;
    processReceived();
    processSelfReceived();
    processAssembledReceived();
    processSent();
    //log(V5_DEBG, "ENDADV\n");
}

bool MessageQueue::hasOpenSends() {
    return !_send_queue.empty();
}

bool MessageQueue::hasOpenRecvFragments() {
    return !_fragmented_messages.empty();
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
            int numFragments = 0;
            {
                auto lock = _garbage_mutex.getLock();
                for (auto& frag : data.dataFragments) if (frag) {
                    _garbage_queue.emplace_back(std::move(frag));
                    numFragments++;
                }
            }
            _garbage_cond_var.notify();
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
            assert(frag);
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

    DataPtr dataPtr;
    while (_gc.continueRunning()) {
        _garbage_cond_var.wait(_garbage_mutex, [&]() {return !_garbage_queue.empty();});
        {
            auto lock = _garbage_mutex.getLock();
            dataPtr = std::move(_garbage_queue.front());
            _garbage_queue.pop_front();
        }
        dataPtr.reset();
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
        const uint8_t* recvData = _active_recv_data;
        const int source = status.MPI_SOURCE;
        int tag = status.MPI_TAG;
        int msglen;
        MPI_Get_count(&status, MPI_BYTE, &msglen);
        LOG(V5_DEBG, "MQ RECV n=%i s=[%i] t=%i c=(%i,...,%i,%i,%i)\n", msglen, source, tag, 
                msglen>=1*sizeof(int) ? *(int*)(recvData) : 0,
                msglen>=3*sizeof(int) ? *(int*)(recvData+msglen - 3*sizeof(int)) : 0,
                msglen>=2*sizeof(int) ? *(int*)(recvData+msglen - 2*sizeof(int)) : 0,
                msglen>=1*sizeof(int) ? *(int*)(recvData+msglen - 1*sizeof(int)) : 0);

        resetReceiveHandle();

        if (tag >= MSG_OFFSET_BATCHED) {
            // Fragment of a message

            tag -= MSG_OFFSET_BATCHED;
            assert(msglen >= 3*sizeof(int));
            int id = ReceiveFragment::readId(recvData, msglen);
            auto key = std::pair<int, int>(source, id);
            
            if (!_fragmented_messages.count(key)) {
                _fragmented_messages.emplace(key, ReceiveFragment(source, id, tag));
            }
            auto& fragment = _fragmented_messages[key];

            fragment.receiveNext(source, tag, recvData, msglen);

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
        _received_handle.setReceive(msglen, recvData);
        _received_handle.tag = tag;
        _received_handle.source = source;

        // Process message according to its tag-specific callback
        *_current_recv_tag = _received_handle.tag;
        digestReceivedMessage(_received_handle);
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
    _active_recv_data = (_active_recv_data == _recv_data_1) ? _recv_data_2 : _recv_data_1;
    MPI_Irecv(_active_recv_data, _max_msg_size+20, MPI_BYTE, MPI_ANY_SOURCE,
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
        _received_handle.tag = sh.tag;
        _received_handle.source = sh.dest;
        _received_handle.setReceive(std::move(*sh.dataPtr));
        *_current_recv_tag = _received_handle.tag;
        digestReceivedMessage(_received_handle);
        signalCompletion(_received_handle.tag, sh.id);
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
            digestReceivedMessage(h);
            *_current_recv_tag = 0;
            
            if (h.getRecvData().size() > _max_msg_size) {
                // Concurrent deallocation of large chunk of data
                {
                    auto lock = _garbage_mutex.getLock();
                    _garbage_queue.emplace_back(new std::vector<uint8_t>(h.moveRecvData()));
                }
                _garbage_cond_var.notify();
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

    // Test each send handle
    auto it = _send_queue.begin();
    while (it != _send_queue.end()) {
        
        SendHandle& h = *it;

        if (!h.isInitiated()) {
            // Message has not been sent yet
            if (_num_concurrent_sends < _max_concurrent_sends) {
                // can initiate sending
                h.sendNext(_max_msg_size);
                _num_concurrent_sends++;
            }
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
            h.printBatchArrived();

            // More batches yet to send?
            if (!h.isFinished()) {
                // Send next batch
                h.sendNext(_max_msg_size);
                completed = false;
            }
        }

        if (completed) {
            // Notify completion
            signalCompletion(h.tag, h.id);
            _num_concurrent_sends--;

            if (h.dataPtr->size() > _max_msg_size) {
                // Concurrent deallocation of SendHandle's large chunk of data
                {
                    auto lock = _garbage_mutex.getLock();
                    _garbage_queue.emplace_back(std::move(h.dataPtr));
                }
                _garbage_cond_var.notify();
            }
            
            // Remove handle
            it = _send_queue.erase(it); // go to next handle
        } else {
            ++it; // go to next handle
        }
    }
}

void MessageQueue::digestReceivedMessage(MessageHandle& h) {

    auto& callbacks = _callbacks.at(h.tag);

    if (callbacks.size() == 1) {
        callbacks.front()(h);
        return;
    }

    for (auto& cb : callbacks) {
        MessageHandle copy(h);
        cb(copy);
    }
}

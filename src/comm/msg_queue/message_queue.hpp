
#ifndef DOMPASCH_MALLOB_MESSAGE_QUEUE_HPP
#define DOMPASCH_MALLOB_MESSAGE_QUEUE_HPP

#include "message_handle.hpp"
#include "receive_fragment.hpp"
#include "send_handle.hpp"

#include <list>
#include <cmath>
#include "util/assert.hpp"
#include <unistd.h>

#include "util/hashing.hpp"
#include "util/sys/background_worker.hpp"
#include "util/logger.hpp"
#include "comm/msgtags.h"
#include "util/sys/atomics.hpp"

class MessageQueue {

public:
    typedef std::function<void(MessageHandle&)> MsgCallback;
    typedef std::function<void(int)> SendDoneCallback;

private:
    size_t _max_msg_size;
    int _my_rank;
    int _comm_size;
    unsigned long long _iteration = 0;

    // Basic receive stuff
    MPI_Request _recv_request;
    uint8_t* _recv_data;
    std::list<SendHandle> _self_recv_queue;
    int _base_num_receives_per_loop = 10;
    int _num_receives_per_loop = _base_num_receives_per_loop;
    MessageHandle _received_handle;

    // Fragmented messages stuff
    robin_hood::unordered_node_map<std::pair<int, int>, ReceiveFragment, IntPairHasher> _fragmented_messages;
    Mutex _fragmented_mutex;
    ConditionVariable _fragmented_cond_var;
    std::list<ReceiveFragment> _fragmented_queue;
    std::atomic_int _num_fused = 0;
    Mutex _fused_mutex;
    std::list<MessageHandle> _fused_queue;

    // Send stuff
    std::list<SendHandle> _send_queue;
    int _running_send_id = 1;
    int _num_concurrent_sends = 0;
    int _max_concurrent_sends = 16;

    // Garbage collection
    std::atomic_int _num_garbage = 0;
    Mutex _garbage_mutex;
    std::list<DataPtr> _garbage_queue;

    // Callbacks
    robin_hood::unordered_map<int, std::list<MsgCallback>> _callbacks;
    robin_hood::unordered_map<int, SendDoneCallback> _send_done_callbacks;
    int _default_tag_var = 0;
    int* _current_recv_tag = nullptr;
    int* _current_send_tag = nullptr;

    BackgroundWorker _batch_assembler;
    BackgroundWorker _gc;

public:
    MessageQueue(int maxMsgSize);
    ~MessageQueue();

    typedef std::list<MsgCallback>::iterator CallbackRef;
    CallbackRef registerCallback(int tag, const MsgCallback& cb);
    void registerSentCallback(int tag, const SendDoneCallback& cb);
    void clearCallbacks();
    void clearCallback(int tag, const CallbackRef& ref);
    void setCurrentTagPointers(int* recvTag, int* sendTag) {
        _current_recv_tag = recvTag;
        _current_send_tag = sendTag;
    }

    int send(const DataPtr& data, int dest, int tag);
    void cancelSend(int sendId);
    void advance();

    bool hasOpenSends();

private:
    void runFragmentedMessageAssembler();
    void runGarbageCollector();

    void processReceived();
    void processSelfReceived();
    void processAssembledReceived();
    void processSent();

    void resetReceiveHandle();
    void signalCompletion(int tag, int id);

    void digestReceivedMessage(MessageHandle& h);
};

#endif

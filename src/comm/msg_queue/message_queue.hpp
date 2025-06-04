
#ifndef DOMPASCH_MALLOB_MESSAGE_QUEUE_HPP
#define DOMPASCH_MALLOB_MESSAGE_QUEUE_HPP

#include <stdint.h>                        // for uint8_t
#include <unistd.h>                        // for size_t
#include <atomic>                          // for atomic_int
#include <functional>                      // for function
#include <list>                            // for list, list<>::iterator
#include <utility>                         // for pair

#include "comm/mpi_base.hpp"               // for MPI_REQUEST_NULL, MPI_Request
#include "message_handle.hpp"              // for MessageHandle
#include "receive_fragment.hpp"            // for ReceiveFragment
#include "send_handle.hpp"                 // for DataPtr, SendHandle
#include "util/hashing.hpp"
#include "util/robin_hood.hpp"             // for unordered_map, unordered_n...
#include "util/sys/background_worker.hpp"  // for BackgroundWorker
#include "util/sys/threading.hpp"          // for Mutex, ConditionVariable

struct IntPairHasher;


class MessageQueue {

public:
    typedef std::function<void(MessageHandle&)> MsgCallback;
    typedef std::function<bool(MessageHandle&)> ConditionalMsgCallback;
    typedef std::function<void(int)> SendDoneCallback;

private:
    size_t _max_msg_size;
    int _my_rank;
    int _comm_size;
    unsigned long long _iteration = 0;

    // Basic receive stuff
    MPI_Request _recv_request {MPI_REQUEST_NULL};
    // Double buffer
    uint8_t* _recv_data_1;
    uint8_t* _recv_data_2;
    uint8_t* _active_recv_data {nullptr};
    std::list<SendHandle> _self_recv_queue;
    int _base_num_receives_per_loop = 10;
    int _num_receives_per_loop = _base_num_receives_per_loop;
    MessageHandle _received_handle;

    // Fragmented messages stuff
    robin_hood::unordered_node_map<std::pair<int, int>, ReceiveFragment, IntPairHasher> _fragmented_messages;
    Mutex _fragmented_mutex;
    ConditionVariable _fragmented_cond_var;
    std::list<ReceiveFragment> _fragmented_queue;
    std::atomic_int _num_fused {0};
    Mutex _fused_mutex;
    std::list<MessageHandle> _fused_queue;

    // Send stuff
    std::list<SendHandle> _send_queue;
    int _running_send_id = 1;
    int _num_concurrent_sends = 0;
    int _max_concurrent_sends = 16;

    // Garbage collection
    Mutex _garbage_mutex;
    ConditionVariable _garbage_cond_var;
    std::list<DataPtr> _garbage_queue;

    // Callbacks
    robin_hood::unordered_map<int, std::list<MsgCallback>> _callbacks;
    robin_hood::unordered_map<int, SendDoneCallback> _send_done_callbacks;
    int _default_tag_var = 0;
    int* _current_recv_tag = nullptr;
    int* _current_send_tag = nullptr;
    robin_hood::unordered_map<int, std::list<ConditionalMsgCallback>> _cond_callbacks;

    BackgroundWorker _batch_assembler;
    BackgroundWorker _gc;

public:
    MessageQueue(int maxMsgSize);
    void close();
    ~MessageQueue();

    typedef std::list<MsgCallback>::iterator CallbackRef;
    typedef std::list<ConditionalMsgCallback>::iterator CondCallbackRef;
    void initializeConditionalCallbacks(int tag);
    CallbackRef registerCallback(int tag, const MsgCallback& cb);
    CondCallbackRef registerConditionalCallback(int tag, const ConditionalMsgCallback& cb);
    void registerSentCallback(int tag, const SendDoneCallback& cb);
    void clearCallbacks();
    void clearCallback(int tag, const CallbackRef& ref);
    void clearConditionalCallback(int tag, const CondCallbackRef& ref);
    void setCurrentTagPointers(int* recvTag, int* sendTag) {
        _current_recv_tag = recvTag;
        _current_send_tag = sendTag;
    }

    int send(const DataPtr& data, int dest, int tag);
    void cancelSend(int sendId);
    void advance();

    bool hasOpenSends();
    bool hasOpenRecvFragments();

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

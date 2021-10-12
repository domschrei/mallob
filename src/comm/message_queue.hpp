
#ifndef DOMPASCH_MALLOB_MESSAGE_QUEUE_HPP
#define DOMPASCH_MALLOB_MESSAGE_QUEUE_HPP

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

typedef std::shared_ptr<std::vector<uint8_t>> DataPtr;
typedef std::shared_ptr<const std::vector<uint8_t>> ConstDataPtr; 

class MessageQueue {
    
private:
    struct ReceiveFragment {
        int source;
        int tag;
        int receivedFragments = 0;
        std::vector<DataPtr> dataFragments;
        ReceiveFragment() = default;
        ReceiveFragment(ReceiveFragment&& moved) {
            source = moved.source;
            tag = moved.tag;
            receivedFragments = moved.receivedFragments;
            dataFragments = std::move(moved.dataFragments);
        }
        ReceiveFragment& operator=(ReceiveFragment&& moved) {
            source = moved.source;
            tag = moved.tag;
            receivedFragments = moved.receivedFragments;
            dataFragments = std::move(moved.dataFragments);
            return *this;
        }
    };

    struct SendHandle {
        int id;
        int dest;
        int tag;
        MPI_Request request;
        DataPtr data;
        int sentBatches = -1;
        int totalNumBatches;
        std::vector<uint8_t> tempStorage;
        
        SendHandle() = default;
        SendHandle(SendHandle&& moved) {
            id = moved.id;
            dest = moved.dest;
            tag = moved.tag;
            request = moved.request;
            data = std::move(moved.data);
            sentBatches = moved.sentBatches;
            totalNumBatches = moved.totalNumBatches;
            tempStorage = std::move(moved.tempStorage);
        }
        SendHandle& operator=(SendHandle&& moved) {
            id = moved.id;
            dest = moved.dest;
            tag = moved.tag;
            request = moved.request;
            data = std::move(moved.data);
            sentBatches = moved.sentBatches;
            totalNumBatches = moved.totalNumBatches;
            tempStorage = std::move(moved.tempStorage);
            return *this;
        }
    };

    size_t _max_msg_size;
    int _my_rank;
    unsigned long long _iteration = 0;

    // Basic receive stuff
    MPI_Request _recv_request;
    uint8_t* _recv_data;
    std::list<SendHandle> _self_recv_queue;

    // Fragmented messages stuff
    robin_hood::unordered_map<std::pair<int, int>, ReceiveFragment, IntPairHasher> _fragmented_messages;
    SPSCRingBuffer<ReceiveFragment> _fragmented_queue;
    SPSCRingBuffer<MessageHandle> _fused_queue;

    // Send stuff
    std::list<SendHandle> sendQueue;
    int runningSendId = 1;

    // Garbage collection
    SPSCRingBuffer<DataPtr> _garbage_queue;

    // Callbacks
    typedef std::function<void(MessageHandle&)> MsgCallback;
    robin_hood::unordered_map<int, MsgCallback> _callbacks;
    std::function<void(int)> _send_done_callback = [](int) {};

    BackgroundWorker _batch_assembler;
    BackgroundWorker _gc;

public:
    MessageQueue(int maxMsgSize);
    ~MessageQueue();

    void registerCallback(int tag, const MsgCallback& cb);
    void registerSentCallback(std::function<void(int)> callback);

    int send(DataPtr data, int dest, int tag);
    void advance();

private:
    void runFragmentedMessageAssembler();
    void runGarbageCollector();

    void processReceived();
    void processSelfReceived();
    void processAssembledReceived();
    void processSent();
    int prepareSendHandleForNextBatch(SendHandle& h);
    size_t getTotalNumBatches(const SendHandle& h) const;
    bool isBatched(const SendHandle& h) const;
    bool isFinished(const SendHandle& h) const;
};

#endif

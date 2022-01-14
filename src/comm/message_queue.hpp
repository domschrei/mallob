
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
#include "util/sys/atomics.hpp"

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
        int sizePerBatch;
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
            sizePerBatch = moved.sizePerBatch;
            tempStorage = std::move(moved.tempStorage);
            
            moved.data = DataPtr();
            moved.request = MPI_REQUEST_NULL;
        }
        SendHandle& operator=(SendHandle&& moved) {
            id = moved.id;
            dest = moved.dest;
            tag = moved.tag;
            request = moved.request;
            data = std::move(moved.data);
            sentBatches = moved.sentBatches;
            totalNumBatches = moved.totalNumBatches;
            sizePerBatch = moved.sizePerBatch;
            tempStorage = std::move(moved.tempStorage);
            
            moved.data = DataPtr();
            moved.request = MPI_REQUEST_NULL;
            return *this;
        }

        int prepareForNextBatch() {
            assert(!isFinished() || log_return_false("Batched handle (n=%i) already finished!\n", sentBatches));

            size_t begin = sentBatches*sizePerBatch;
            size_t end = std::min(data->size(), (size_t)(sentBatches+1)*sizePerBatch);
            assert(end>begin || log_return_false("%ld <= %ld\n", end, begin));
            size_t msglen = (end-begin)+3*sizeof(int);
            tempStorage.resize(msglen);

            // Copy actual data
            memcpy(tempStorage.data(), data->data()+begin, end-begin);
            // Copy meta data at insertion point
            memcpy(tempStorage.data()+(end-begin), &id, sizeof(int));
            memcpy(tempStorage.data()+(end-begin)+sizeof(int), &sentBatches, sizeof(int));
            memcpy(tempStorage.data()+(end-begin)+2*sizeof(int), &totalNumBatches, sizeof(int));

            return tag + MSG_OFFSET_BATCHED;
        }
        bool isBatched() const {return sentBatches >= 0;}
        size_t getTotalNumBatches() const {assert(isBatched()); return std::ceil(data->size() / (float)sizePerBatch);}
        bool isFinished() const {assert(isBatched()); return sentBatches == totalNumBatches;}
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
    std::atomic_int _num_fused = 0;
    Mutex _fused_mutex;
    std::list<MessageHandle> _fused_queue;

    // Send stuff
    std::list<SendHandle> _send_queue;
    int _running_send_id = 1;

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
    void clearCallbacks();

    int send(DataPtr data, int dest, int tag);
    void advance();

private:
    void runFragmentedMessageAssembler();
    void runGarbageCollector();

    void processReceived();
    void processSelfReceived();
    void processAssembledReceived();
    void processSent();
};

#endif

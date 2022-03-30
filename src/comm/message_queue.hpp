
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
#include "util/sys/atomics.hpp"

typedef std::shared_ptr<std::vector<uint8_t>> DataPtr;
typedef std::unique_ptr<std::vector<uint8_t>> UniqueDataPtr;
typedef std::shared_ptr<const std::vector<uint8_t>> ConstDataPtr; 

class MessageQueue {
    
private:
    struct ReceiveFragment {
        
        int source = -1;
        int id = -1;
        int tag = -1;
        int receivedFragments = 0;
        std::vector<UniqueDataPtr> dataFragments;
        bool cancelled = false;
        
        ReceiveFragment() = default;
        ReceiveFragment(int source, int id, int tag) : source(source), id(id), tag(tag) {}

        ReceiveFragment(ReceiveFragment&& moved) {
            source = moved.source;
            id = moved.id;
            tag = moved.tag;
            receivedFragments = moved.receivedFragments;
            dataFragments = std::move(moved.dataFragments);
            cancelled = moved.cancelled;
            moved.id = -1;
        }
        ReceiveFragment& operator=(ReceiveFragment&& moved) {
            source = moved.source;
            id = moved.id;
            tag = moved.tag;
            receivedFragments = moved.receivedFragments;
            dataFragments = std::move(moved.dataFragments);
            cancelled = moved.cancelled;
            moved.id = -1;
            return *this;
        }

        bool valid() const {return id != -1;}
        bool isCancelled() const {return cancelled;}

        static int readId(uint8_t* data, int msglen) {
            return * (int*) (data+msglen - 3*sizeof(int));
        }

        void receiveNext(int source, int tag, uint8_t* data, int msglen) {
            assert(this->source >= 0);
            assert(valid());

            int id, sentBatch, totalNumBatches;
            // Read meta data from end of message
            memcpy(&id,              data+msglen - 3*sizeof(int), sizeof(int));
            memcpy(&sentBatch,       data+msglen - 2*sizeof(int), sizeof(int));
            memcpy(&totalNumBatches, data+msglen - 1*sizeof(int), sizeof(int));
            msglen -= 3*sizeof(int);
            
            if (msglen == 0 && sentBatch == 0 && totalNumBatches == 0) {
                // Message was cancelled!
                cancelled = true;
                return;
            }

            if (sentBatch == 0 || sentBatch+1 == totalNumBatches) {
                LOG(V4_VVER, "RECVB %i %i/%i %i\n", id, sentBatch+1, totalNumBatches, source);
            } else {
                LOG(V5_DEBG, "RECVB %i %i/%i %i\n", id, sentBatch+1, totalNumBatches, source);
            }

            // Store data in fragments structure
            
            //log(V5_DEBG, "MQ STORE (%i,%i) %i/%i\n", source, id, sentBatch, totalNumBatches);

            assert(this->source == source);
            assert(this->id == id || LOG_RETURN_FALSE("%i != %i\n", this->id, id));
            assert(this->tag == tag);
            assert(sentBatch < totalNumBatches || LOG_RETURN_FALSE("Invalid batch %i/%i!\n", sentBatch, totalNumBatches));
            if (totalNumBatches > dataFragments.size()) dataFragments.resize(totalNumBatches);
            assert(receivedFragments >= 0 || LOG_RETURN_FALSE("Batched message was already completed!\n"));

            //log(V5_DEBG, "MQ STORE alloc\n");
            assert(dataFragments[sentBatch] == nullptr || LOG_RETURN_FALSE("Batch %i/%i already present!\n", sentBatch, totalNumBatches));
            dataFragments[sentBatch].reset(new std::vector<uint8_t>(data, data+msglen));
            
            //log(V5_DEBG, "MQ STORE produce\n");
            // All fragments of the message received?
            receivedFragments++;
            if (receivedFragments == totalNumBatches)
                receivedFragments = -1;
        }

        bool isFinished() {
            assert(valid());
            return receivedFragments == -1;
        }
    };

    struct SendHandle {

        int id = -1;
        int dest;
        int tag;
        MPI_Request request = MPI_REQUEST_NULL;
        DataPtr data;
        int sentBatches = -1;
        int totalNumBatches;
        int sizePerBatch;
        std::vector<uint8_t> tempStorage;
        
        SendHandle(int id, int dest, int tag, DataPtr data, int maxMsgSize) 
            : id(id), dest(dest), tag(tag), data(data) {

            sizePerBatch = maxMsgSize;
            sentBatches = 0;
            totalNumBatches = data->size() <= sizePerBatch+3*sizeof(int) ? 1 
                : std::ceil(data->size() / (float)sizePerBatch);
        }

        bool valid() {return id != -1;}
        
        SendHandle(SendHandle&& moved) {
            assert(moved.valid());
            id = moved.id;
            dest = moved.dest;
            tag = moved.tag;
            request = moved.request;
            data = std::move(moved.data);
            sentBatches = moved.sentBatches;
            totalNumBatches = moved.totalNumBatches;
            sizePerBatch = moved.sizePerBatch;
            tempStorage = std::move(moved.tempStorage);
            
            moved.id = -1;
            moved.data = DataPtr();
            moved.request = MPI_REQUEST_NULL;
        }
        SendHandle& operator=(SendHandle&& moved) {
            assert(moved.valid());
            id = moved.id;
            dest = moved.dest;
            tag = moved.tag;
            request = moved.request;
            data = std::move(moved.data);
            sentBatches = moved.sentBatches;
            totalNumBatches = moved.totalNumBatches;
            sizePerBatch = moved.sizePerBatch;
            tempStorage = std::move(moved.tempStorage);
            
            moved.id = -1;
            moved.data = DataPtr();
            moved.request = MPI_REQUEST_NULL;
            return *this;
        }

        bool isInitiated() {
            return request != MPI_REQUEST_NULL;
        }

        bool test() {
            assert(valid());
            assert(request != MPI_REQUEST_NULL);
            int flag = false;
            MPI_Test(&request, &flag, MPI_STATUS_IGNORE);
            return flag;
        }

        bool isFinished() const {return sentBatches == totalNumBatches;}

        void sendNext() {
            assert(valid());
            assert(!isFinished() || LOG_RETURN_FALSE("Handle (n=%i) already finished!\n", sentBatches));
            
            if (!isBatched()) {
                // Send first and only message
                //log(V5_DEBG, "MQ SEND SINGLE id=%i\n", id);
                MPI_Isend(data->data(), data->size(), MPI_BYTE, dest, tag, MPI_COMM_WORLD, &request);
                sentBatches = 1;
                return;
            }

            if (isCancelled()) {
                int zero = 0;
                if (tempStorage.size() < 3*sizeof(int)) tempStorage.resize(3*sizeof(int));
                memcpy(tempStorage.data(), &id, sizeof(int));
                memcpy(tempStorage.data()+sizeof(int), &zero, sizeof(int));
                memcpy(tempStorage.data()+2*sizeof(int), &zero, sizeof(int));
                MPI_Isend(tempStorage.data(), 3*sizeof(int), MPI_BYTE, 
                    dest, tag+MSG_OFFSET_BATCHED, MPI_COMM_WORLD, &request);
                sentBatches = totalNumBatches; // mark as finished
                return;
            }

            size_t begin = sentBatches*sizePerBatch;
            size_t end = std::min(data->size(), (size_t)(sentBatches+1)*sizePerBatch);
            assert(end>begin || LOG_RETURN_FALSE("%ld <= %ld\n", end, begin));
            size_t msglen = (end-begin)+3*sizeof(int);
            if (msglen > tempStorage.size()) tempStorage.resize(msglen);

            // Copy actual data
            memcpy(tempStorage.data(), data->data()+begin, end-begin);
            // Copy meta data at insertion point
            memcpy(tempStorage.data()+(end-begin), &id, sizeof(int));
            memcpy(tempStorage.data()+(end-begin)+sizeof(int), &sentBatches, sizeof(int));
            memcpy(tempStorage.data()+(end-begin)+2*sizeof(int), &totalNumBatches, sizeof(int));

            MPI_Isend(tempStorage.data(), msglen, MPI_BYTE, dest, 
                    tag+MSG_OFFSET_BATCHED, MPI_COMM_WORLD, &request);

            sentBatches++;
            if (sentBatches == 1 || sentBatches == totalNumBatches) {
                LOG(V4_VVER, "SENDB %i %i/%i %i\n", id, sentBatches, totalNumBatches, dest);
            } else {
                LOG(V5_DEBG, "SENDB %i %i/%i %i\n", id, sentBatches, totalNumBatches, dest);
            }
            //log(V5_DEBG, "MQ SEND BATCHED id=%i %i/%i\n", id, sentBatches, totalNumBatches);
        }

        void cancel() {
            sizePerBatch = -1;
        }

        bool isBatched() const {return totalNumBatches > 1;}
        bool isCancelled() const {return sizePerBatch == -1;}
        size_t getTotalNumBatches() const {assert(isBatched()); return totalNumBatches;}
    };

    size_t _max_msg_size;
    int _my_rank;
    unsigned long long _iteration = 0;

    // Basic receive stuff
    MPI_Request _recv_request;
    uint8_t* _recv_data;
    std::list<SendHandle> _self_recv_queue;
    int _base_num_receives_per_loop = 10;
    int _num_receives_per_loop = _base_num_receives_per_loop;

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
    typedef std::function<void(MessageHandle&)> MsgCallback;
    typedef std::function<void(int)> SendDoneCallback;
    robin_hood::unordered_map<int, MsgCallback> _callbacks;
    robin_hood::unordered_map<int, SendDoneCallback> _send_done_callbacks;
    int _default_tag_var = 0;
    int* _current_recv_tag = nullptr;
    int* _current_send_tag = nullptr;

    BackgroundWorker _batch_assembler;
    BackgroundWorker _gc;

public:
    MessageQueue(int maxMsgSize);
    ~MessageQueue();

    void registerCallback(int tag, const MsgCallback& cb);
    void registerSentCallback(int tag, const SendDoneCallback& cb);
    void clearCallbacks();
    void setCurrentTagPointers(int* recvTag, int* sendTag) {
        _current_recv_tag = recvTag;
        _current_send_tag = sendTag;
    }

    int send(DataPtr data, int dest, int tag);
    void cancelSend(int sendId);
    void advance();

private:
    void runFragmentedMessageAssembler();
    void runGarbageCollector();

    void processReceived();
    void processSelfReceived();
    void processAssembledReceived();
    void processSent();

    void resetReceiveHandle();
    void signalCompletion(int tag, int id);
};

#endif

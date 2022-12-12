
#pragma once

#include <vector>
#include <list>
#include <cstring>
#include <memory>

#include "util/assert.hpp"
#include "util/logger.hpp"

struct ReceiveFragment {

    int source = -1;
    int id = -1;
    int tag = -1;
    int receivedFragments = 0;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> dataFragments;
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
        auto& frag = dataFragments[sentBatch];
        assert(!frag || LOG_RETURN_FALSE("Batch %i/%i already present!\n", sentBatch, totalNumBatches));
        frag.reset(new std::vector<uint8_t>(data, data+msglen));
        
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

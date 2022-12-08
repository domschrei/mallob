
#pragma once

#include <vector>
#include <cmath>
#include <cstring>
#include <memory>

#include "util/assert.hpp"
#include "comm/mpi_base.hpp"
#include "util/logger.hpp"
#include "comm/msgtags.h"

typedef std::shared_ptr<std::vector<uint8_t>> DataPtr;
typedef std::unique_ptr<std::vector<uint8_t>> UniqueDataPtr;
typedef std::shared_ptr<const std::vector<uint8_t>> ConstDataPtr;

struct SendHandle {

    int id = -1;
    int dest;
    int tag;
    MPI_Request request = MPI_REQUEST_NULL;
    DataPtr dataPtr;
    int sentBatches = -1;
    int totalNumBatches;
    bool cancelled {false};
    std::vector<uint8_t> tempStorage;
    
    SendHandle(int id, int dest, int tag, const DataPtr& sendData, int maxMsgSize) 
        : id(id), dest(dest), tag(tag), dataPtr(sendData) {

        auto& data = *dataPtr;
        auto sizePerBatch = maxMsgSize;
        sentBatches = 0;
        totalNumBatches = data.size() <= sizePerBatch+3*sizeof(int) ? 1 
            : std::ceil(data.size() / (float)sizePerBatch);
    }

    bool valid() {return id != -1;}
    
    SendHandle(SendHandle&& moved) {
        assert(moved.valid());
        id = moved.id;
        dest = moved.dest;
        tag = moved.tag;
        request = moved.request;
        dataPtr = std::move(moved.dataPtr);
        sentBatches = moved.sentBatches;
        totalNumBatches = moved.totalNumBatches;
        cancelled = moved.cancelled;
        tempStorage = std::move(moved.tempStorage);
        
        moved.id = -1;
        moved.request = MPI_REQUEST_NULL;
    }
    SendHandle& operator=(SendHandle&& moved) {
        assert(moved.valid());
        id = moved.id;
        dest = moved.dest;
        tag = moved.tag;
        request = moved.request;
        dataPtr = std::move(moved.dataPtr);
        sentBatches = moved.sentBatches;
        totalNumBatches = moved.totalNumBatches;
        cancelled = moved.cancelled;
        tempStorage = std::move(moved.tempStorage);
        
        moved.id = -1;
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

    void sendNext(int sizePerBatch) {

        assert(valid());
        assert(!isFinished() || LOG_RETURN_FALSE("Handle (n=%i) already finished!\n", sentBatches));
        auto& data = *dataPtr;

        if (!isBatched()) {
            // Send first and only message
            //log(V5_DEBG, "MQ SEND SINGLE id=%i\n", id);
            MPI_Isend(data.data(), data.size(), MPI_BYTE, dest, tag, MPI_COMM_WORLD, &request);
            sentBatches = 1;
            return;
        }

        if (isCancelled()) {
            // Send cancelling message
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

        size_t begin = ((size_t)sentBatches)*sizePerBatch;
        size_t end = std::min(data.size(), ((size_t)(sentBatches+1))*sizePerBatch);
        assert(end>begin || LOG_RETURN_FALSE("%ld <= %ld\n", end, begin));
        size_t msglen = (end-begin)+3*sizeof(int);
        if (msglen > tempStorage.size()) tempStorage.resize(msglen);

        // Copy actual data
        memcpy(tempStorage.data(), data.data()+begin, end-begin);
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
        cancelled = true;
    }

    bool isBatched() const {return totalNumBatches > 1;}
    bool isCancelled() const {return cancelled;}
    size_t getTotalNumBatches() const {assert(isBatched()); return totalNumBatches;}

    void printSendMsg() const {

        auto& data = *dataPtr;
        int msglen = data.size();

#if LOGGER_STATIC_VERBOSITY >= 6
        if (Logger::getMainInstance().getVerbosity() >= 6) {
            std::string msgContent;
            size_t i = 0;
            while (i + sizeof(int) <= msglen) {
                msgContent += std::to_string(*(int*)(data.data()+i)) + ",";
                i += sizeof(int);
            }
            msgContent = msgContent.substr(0, msgContent.size()-1);
            LOG(V5_DEBG, "MQ SEND n=%i d=[%i] t=%i c=(%s)\n", data.size(), dest, tag, msgContent.c_str());
        } else
#endif
        LOG(V5_DEBG, "MQ SEND n=%i d=[%i] t=%i c=(%i,...,%i,%i,%i)\n", data.size(), dest, tag, 
            msglen>=1*sizeof(int) ? *(int*)(data.data()) : 0, 
            msglen>=3*sizeof(int) ? *(int*)(data.data()+msglen - 3*sizeof(int)) : 0, 
            msglen>=2*sizeof(int) ? *(int*)(data.data()+msglen - 2*sizeof(int)) : 0, 
            msglen>=1*sizeof(int) ? *(int*)(data.data()+msglen - 1*sizeof(int)) : 0);
        assert(dest >= 0 && dest < 1'000'000);
    }

    void printBatchArrived() const {
        LOG(V5_DEBG, "MQ SENT id=%i %i/%i n=%i d=[%i] t=%i c=(%i,...,%i,%i,%i)\n", id, sentBatches, 
                totalNumBatches, dataPtr->size(), dest, tag, 
                *(int*)(tempStorage.data()), 
                *(int*)(tempStorage.data()+tempStorage.size()-3*sizeof(int)), 
                *(int*)(tempStorage.data()+tempStorage.size()-2*sizeof(int)),
                *(int*)(tempStorage.data()+tempStorage.size()-1*sizeof(int)));
    }
};
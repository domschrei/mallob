
#pragma once

#include <stddef.h>                        // for size_t
#include <stdint.h>                        // for uint8_t
#include <vector>                          // for vector

#include "comm/mpi_base.hpp"               // for MPI_Comm, MPI_Request, MPI_Op
#include "comm/msg_queue/send_handle.hpp"  // for DataPtr

class MessageQueue;
class Parameters;  // lines 13-13
class Serializable;  // lines 12-12

#define MIN_PRIORITY 0

class Serializable;
class Parameters;

class MyMpi {

public:    
    /*
    struct RecvBundle {int source; int tag; MPI_Comm comm;};
    static ConcurrentAllocator<RecvBundle> _alloc;
    */
    static MessageQueue* _msg_queue;

    static void init();
    static void setOptions(const Parameters& params);

    static int isend(int recvRank, int tag, const Serializable& object);
    static int isend(int recvRank, int tag, std::vector<uint8_t>&& object);
    static int isend(int recvRank, int tag, const DataPtr& object);
    static int isendCopy(int recvRank, int tag, const std::vector<uint8_t>& object);
    
    static MPI_Request    ireduce(MPI_Comm communicator, float* contribution, float* result, int rootRank, MPI_Op operation = MPI_SUM);
    static MPI_Request iallreduce(MPI_Comm communicator, float* contribution, float* result, MPI_Op operation = MPI_SUM);
    static MPI_Request iallreduce(MPI_Comm communicator, float* contribution, float* result, int numFloats, MPI_Op operation = MPI_SUM);

    static MPI_Request iallgather(MPI_Comm communicator, float* contribution, float* result, int numFloats);

    static int size(MPI_Comm comm);
    static int rank(MPI_Comm comm);

    static MessageQueue& getMessageQueue();

    static void broadcastExitSignal();
};

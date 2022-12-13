
#pragma once

#include <memory>
#include <set>
#include <map>

#include "comm/mpi_base.hpp"
#include "comm/msg_queue/message_handle.hpp"
#include "comm/msg_queue/message_queue.hpp"
#include "data/serializable.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/concurrent_allocator.hpp"
#include "util/hashing.hpp"

#include "msgtags.h"

#define MIN_PRIORITY 0

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

    enum BufferQueryMode {SELF, ALL};
    static size_t getBinaryTreeBufferLimit(int numWorkers, int baseSize, float discountFactor, BufferQueryMode mode);

    static int size(MPI_Comm comm);
    static int rank(MPI_Comm comm);

    static MessageQueue& getMessageQueue();
};

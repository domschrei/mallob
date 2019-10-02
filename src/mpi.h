
#ifndef DOMPASCH_CUCKOO_BALANCER_BOOSTMPI
#define DOMPASCH_CUCKOO_BALANCER_BOOSTMPI

#include <iostream>
#include <memory>
#include <set>
#include <assert.h>
#include <openmpi/mpi.h>

#include "serializable.h"

#define BROADCAST_CLAUSE_INTS_PER_NODE 1500

struct MessageHandle {
    MPI_Request request;
    int tag;
    int source;
    std::vector<int> sendData;
    std::vector<int> recvData;
    MPI_Status status;
    bool selfMessage = false;

    MessageHandle() {}
    MessageHandle(const std::vector<int>& data) : sendData(data) {}
};

const int MSG_QUERY_LOAD = 1;
const int MSG_ANSWER_LOAD = 2;
const int MSG_FIND_NODE = 3;
const int MSG_REQUEST_BECOME_CHILD = 4;
const int MSG_ACCEPT_BECOME_CHILD = 5;
const int MSG_REJECT_BECOME_CHILD = 19;
const int MSG_ACK_ACCEPT_BECOME_CHILD = 6;
const int MSG_UPDATE_DEMAND = 7;
const int MSG_TERMINATED = 8;
const int MSG_RESULT = 9;
const int MSG_SEND_JOB = 10;
const int MSG_INTRODUCE_JOB = 11;
const int MSG_CHECK_NODE_PERMUTATION = 12;
const int MSG_CONFIRM_NODE_PERMUTATION = 13;
const int MSG_ADJUST_NODE_PERMUTATION = 14;
const int MSG_WORKER_FOUND_RESULT = 15;
const int MSG_TERMINATE = 16;
const int MSG_GATHER_CLAUSES = 17;
const int MSG_DISTRIBUTE_CLAUSES = 18;

typedef std::shared_ptr<MessageHandle> MessageHandlePtr;

class MyMpi {

private:
    static std::set<MessageHandlePtr> handles;
    static std::set<MessageHandlePtr> sentHandles;

public:

    static MessageHandlePtr isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static MessageHandlePtr isend(MPI_Comm communicator, int recvRank, int tag, int object);
    static MessageHandlePtr isend(MPI_Comm communicator, int recvRank, int tag, const std::vector<int>& object);
    static MessageHandlePtr  send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static MessageHandlePtr  send(MPI_Comm communicator, int recvRank, int tag, const std::vector<int>& object);
    static MessageHandlePtr irecv(MPI_Comm communicator);
    static MessageHandlePtr irecv(MPI_Comm communicator, int tag);
    static MessageHandlePtr irecv(MPI_Comm communicator, int source, int tag, int size);
    static MessageHandlePtr recv(MPI_Comm communicator, int tag, int size);

    static MessageHandlePtr poll();
    static inline bool hasActiveHandles() {return handles.size() > 0;};
    static void cleanSentHandles();

    static int size(MPI_Comm comm);
    static int rank(MPI_Comm comm);
    static int random_other_node(MPI_Comm comm, const std::set<int>& excludedNodes);

    static void init(int argc, char *argv[]);

    static int maxMsgLength;
};

#endif


#ifndef DOMPASCH_CUCKOO_BALANCER_BOOSTMPI
#define DOMPASCH_CUCKOO_BALANCER_BOOSTMPI

#include <iostream>
#include <memory>
#include <set>
#include <assert.h>
#include <openmpi/mpi.h>

#include "data/serializable.h"

#define MAX_JOB_MESSAGE_PAYLOAD_PER_NODE 1500*sizeof(int)

struct MessageHandle {
    MPI_Request request;
    int tag;
    int source;
    std::vector<uint8_t> sendData;
    std::vector<uint8_t> recvData;
    MPI_Status status;
    bool selfMessage = false;
    bool critical = false;

    MessageHandle() {}
    MessageHandle(const std::vector<uint8_t>& data) : sendData(data) {}
};

/*
 * The receiver is queried to begin working as the i-th node of job j.
 * Data type: JobRequest
 */
const int MSG_FIND_NODE = 3;
/*
 * The sender asks the receiver to become the sender's parent for some job j
 * of which a corresponding child position was advertised.
 * Data type: JobRequest
 */
const int MSG_REQUEST_BECOME_CHILD = 4;
/*
 * The senders confirms that the receiver may become the sender's child
 * with respect to the job and index specified in the signature.
 * Data type: JobSignature
 */
const int MSG_ACCEPT_BECOME_CHILD = 5;
/*
 * The sender rejects the receiver to become the sender's child
 * with respect to the job and index specified in the signature.
 * Data type: JobRequest
 */
const int MSG_REJECT_BECOME_CHILD = 6;
/*
 * The sender acknowledges that it received the receiver's previous
 * MSG_ACCEPT_BECOME_CHILD message.
 * Data type: JobRequest
 */
const int MSG_ACK_ACCEPT_BECOME_CHILD = 7;
/*
 * The sender propagates a job's volume update to the receiver.
 * Data type: [jobId, volume]
 */
const int MSG_UPDATE_VOLUME = 8;
/*
 * The sender transfers a full job description to the receiver.
 * Data type: JobDescription
 * Warning: Length may exceed the default maximum message length.
 */
const int MSG_SEND_JOB = 11;
/*
 * The sender informs the receiver that a solution was found for the job
 * of the specified ID.
 * Data type: [jobId, resultCode]
 */
const int MSG_WORKER_FOUND_RESULT = 16;
/*
 * The sender provides the global rank of the client node which initiated
 * a certain job.
 * Data type: [jobId, clientRank]
 */
const int MSG_FORWARD_CLIENT_RANK = 17;
/*
 * A signal to terminate a job is propagated.
 * Data type: [jobId]
 */
const int MSG_TERMINATE = 18;
/*
 * The sender informs the receiver (a client) that a job has been finished,
 * and also provides the size of the upcoming job result message.
 * Data type: [jobId, sizeOfResult]
 */
const int MSG_JOB_DONE = 19;
/*
 * The sender (a client) acknowledges that it received the receiver's MSG_JOB_DONE
 * message and signals that it wishes to receive the full job result.
 * Data type: [jobId, sizeOfResult]
 */
const int MSG_QUERY_JOB_RESULT = 20;
/*
 * The sender provides a job's full result to the receiver (a client).
 * Data type: JobResult
 * Warning: Length may exceed the default maximum message length.
 */
const int MSG_SEND_JOB_RESULT = 21;
/**
 * The sender (a worker node) informs the receiver (the job's root node) that 
 * the sender is defecting to another job.
 * Data type: [jobId, index]
 */
const int MSG_WORKER_DEFECTING = 22;

const int MSG_COLLECTIVES = 300;
const int MSG_JOB_COMMUNICATION = 400;

typedef std::shared_ptr<MessageHandle> MessageHandlePtr;

class MyMpi {

private:
    static std::set<MessageHandlePtr> handles;
    static std::set<MessageHandlePtr> sentHandles;

public:

    static MessageHandlePtr isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static MessageHandlePtr isend(MPI_Comm communicator, int recvRank, int tag, const std::vector<uint8_t>& object);
    static MessageHandlePtr  send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static MessageHandlePtr  send(MPI_Comm communicator, int recvRank, int tag, const std::vector<uint8_t>& object);
    static MessageHandlePtr irecv(MPI_Comm communicator);
    static MessageHandlePtr irecv(MPI_Comm communicator, int tag);
    static MessageHandlePtr irecv(MPI_Comm communicator, int source, int tag);
    static MessageHandlePtr irecv(MPI_Comm communicator, int source, int tag, int size);
    static MessageHandlePtr  recv(MPI_Comm communicator, int tag, int size);
    static MessageHandlePtr  recv(MPI_Comm communicator, int tag);

    static MessageHandlePtr poll();
    static inline bool hasActiveHandles() {
        return handles.size() > 0;
    }
    static bool hasCriticalHandles();
    static void listen();
    static void deferHandle(MessageHandlePtr handle);
    static void cleanSentHandles();

    static int size(MPI_Comm comm);
    static int rank(MPI_Comm comm);
    static int random_other_node(MPI_Comm comm, const std::set<int>& excludedNodes);

    static void init(int argc, char *argv[]);

    static int maxMsgLength;
};

#endif

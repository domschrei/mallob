
#ifndef DOMPASCH_CUCKOO_BALANCER_BOOSTMPI
#define DOMPASCH_CUCKOO_BALANCER_BOOSTMPI

#include <iostream>
#include <memory>
#include <set>
#include <map>
#include <assert.h>
#include <include/mpi.h>

#include "data/serializable.h"
#include "util/console.h"

#define MAX_JOB_MESSAGE_PAYLOAD_PER_NODE 1500*sizeof(int)
#define MIN_PRIORITY 0

struct MessageHandle {
    MPI_Request request;
    int id;
    int tag;
    int source;
    std::shared_ptr<std::vector<uint8_t>> sendData;
    std::shared_ptr<std::vector<uint8_t>> recvData;
    MPI_Status status;
    bool selfMessage = false;
    bool critical = false;

    MessageHandle(int id) : id(id) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1; 
        sendData = std::make_shared<std::vector<uint8_t>>();
        recvData = std::make_shared<std::vector<uint8_t>>();
        Console::log(Console::VVVERB, "Msg ID=%i created", id);
    }
    MessageHandle(int id, const std::shared_ptr<std::vector<uint8_t>>& data) : id(id), sendData(data) {
        recvData = std::make_shared<std::vector<uint8_t>>();
        Console::log(Console::VVVERB, "Msg ID=%i created", id);
    }
    MessageHandle(int id, const std::shared_ptr<std::vector<uint8_t>>& sendData, const std::shared_ptr<std::vector<uint8_t>>& recvData) : 
        id(id), sendData(sendData), recvData(recvData) {
        Console::log(Console::VVVERB, "Msg ID=%i created", id);
    }
    ~MessageHandle() {
        sendData = NULL;
        recvData = NULL;
        Console::log(Console::VVVERB, "Msg ID=%i deleted", id);
    }
};

const int MSG_WARMUP = 1;
/*
 * The sender wishes to receive the current volume of job j from the receiver.
 * Data type: 1 int (jobId)
 */
const int MSG_QUERY_VOLUME = 2;
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
const int MSG_SEND_JOB_DESCRIPTION = 11;
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

const int MSG_NOTIFY_JOB_REVISION = 24;
const int MSG_QUERY_JOB_REVISION_DETAILS = 25;
const int MSG_SEND_JOB_REVISION_DETAILS = 26;
const int MSG_ACK_JOB_REVISION_DETAILS = 27;
const int MSG_SEND_JOB_REVISION_DATA = 28;
const int MSG_INCREMENTAL_JOB_FINISHED = 29;
const int MSG_INTERRUPT = 30;
const int MSG_ABORT = 31;
const int MSG_CLIENT_FINISHED = 32;
const int MSG_EXIT = 33;

const int MSG_COLLECTIVES = 300;
const int MSG_JOB_COMMUNICATION = 400;


/**
 * All types of messages which can be receivable by a worker node at any time 
 * by a generic irecv method and within the maximum message length.
 */
const int ANYTIME_WORKER_RECV_TAGS[] = {MSG_QUERY_VOLUME, MSG_FIND_NODE, MSG_REQUEST_BECOME_CHILD, MSG_REJECT_BECOME_CHILD, 
            MSG_ACCEPT_BECOME_CHILD, MSG_ACK_ACCEPT_BECOME_CHILD, MSG_UPDATE_VOLUME, MSG_WORKER_FOUND_RESULT, 
            MSG_WORKER_DEFECTING, MSG_FORWARD_CLIENT_RANK, MSG_TERMINATE, MSG_INTERRUPT, MSG_ABORT, MSG_QUERY_JOB_RESULT, 
            MSG_NOTIFY_JOB_REVISION, MSG_QUERY_JOB_REVISION_DETAILS, MSG_SEND_JOB_REVISION_DETAILS, MSG_ACK_JOB_REVISION_DETAILS,
            MSG_JOB_COMMUNICATION, MSG_WARMUP, MSG_EXIT};
/**
 * All types of messages which can be receivable by a client node at any time 
 * by a generic irecv method and within the maximum message length.
 */
const int ANYTIME_CLIENT_RECV_TAGS[] = {MSG_JOB_DONE, MSG_REQUEST_BECOME_CHILD, MSG_ACK_ACCEPT_BECOME_CHILD, 
            MSG_QUERY_JOB_REVISION_DETAILS, MSG_ACK_JOB_REVISION_DETAILS, MSG_ABORT, MSG_CLIENT_FINISHED};

/**
 * Which types of messages the MPI node should listen to. 
 */
enum ListenerMode {CLIENT, WORKER};

/**
 * A std::shared_ptr around a MessageHandle instance which captures all relevant information
 * on a specific MPI message.
 */
typedef std::shared_ptr<MessageHandle> MessageHandlePtr;

class MyMpi {

public:
    struct HandleComparator {
        bool operator()(const MessageHandlePtr& a, const MessageHandlePtr& b) const {
            assert(MyMpi::tagPriority.count(a->tag) || Console::fail("Tag %i has no priority assigned to it", a->tag));
            assert(MyMpi::tagPriority.count(b->tag) || Console::fail("Tag %i has no priority assigned to it", b->tag));
            if (MyMpi::tagPriority[a->tag] != MyMpi::tagPriority[b->tag])
                return MyMpi::tagPriority[a->tag] < MyMpi::tagPriority[b->tag];
            if (a->tag != b->tag) return a->tag < b->tag;
            return false;
        }
    };

private:
    static std::set<MessageHandlePtr> handles;
    static std::set<MessageHandlePtr> sentHandles;
    static std::map<int, int> tagPriority;

public:

    static void init(int argc, char *argv[]);
    static void beginListening(const ListenerMode& mode);
    static void resetListenerIfNecessary(const ListenerMode& mode, int tag);

    static MessageHandlePtr isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static MessageHandlePtr isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object);
    static MessageHandlePtr  send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static MessageHandlePtr  send(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object);
    static MessageHandlePtr irecv(MPI_Comm communicator);
    static MessageHandlePtr irecv(MPI_Comm communicator, int tag);
    static MessageHandlePtr irecv(MPI_Comm communicator, int source, int tag);
    static MessageHandlePtr irecv(MPI_Comm communicator, int source, int tag, int size);
    static MessageHandlePtr  recv(MPI_Comm communicator, int tag, int size);
    static MessageHandlePtr  recv(MPI_Comm communicator, int tag);

    static MessageHandlePtr poll();
    //static MessageHandlePtr pollByProbing(const ListenerMode& mode);
    static inline bool hasActiveHandles() {
        return handles.size() > 0;
    }
    static bool hasOpenSentHandles();
    static void deferHandle(MessageHandlePtr handle);
    static void testSentHandles();

    static int size(MPI_Comm comm);
    static int rank(MPI_Comm comm);
    static int random_other_node(MPI_Comm comm, const std::set<int>& excludedNodes);
    
    static int nextHandleId();

    static int maxMsgLength;
};

#endif


#ifndef DOMPASCH_CUCKOO_BALANCER_BOOSTMPI
#define DOMPASCH_CUCKOO_BALANCER_BOOSTMPI

#include <iostream>
#include <memory>
#include <set>
#include <map>
#include <assert.h>
#include <mpi.h>

#include "data/serializable.hpp"
#include "util/console.hpp"
#include "util/sys/timer.hpp"

#include "msgtags.h"

#define MAX_JOB_MESSAGE_PAYLOAD_PER_NODE 1500*sizeof(int)
#define MAX_ANYTIME_MESSAGE_SIZE 1024
#define MIN_PRIORITY 0

/*
Struct representing one of the tags defined in msgtags.h.
Future versions may extend this struct with additional meta information
(e.g. maximum message size) per tag.
*/
struct MsgTag {
    int id;
    bool anytime;
    MsgTag() {}
    MsgTag(int id, bool anytime) : id(id), anytime(anytime) {}
};

/*
Represents a single message that is being sent or received.
*/
struct MessageHandle {
    int id;
    int tag;
    int source;
    std::shared_ptr<std::vector<uint8_t>> sendData;
    std::shared_ptr<std::vector<uint8_t>> recvData;
    bool selfMessage = false;
    bool finished = false;
    float creationTime = 0;
    MPI_Request request;
    MPI_Status status;

    MessageHandle(int id) : id(id) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1; 
        sendData = std::make_shared<std::vector<uint8_t>>();
        recvData = std::make_shared<std::vector<uint8_t>>();
        creationTime = Timer::elapsedSeconds();
        //Console::log(Console::VVVVERB, "Msg ID=%i created", id);
    }
    MessageHandle(int id, int recvSize) : id(id) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1; 
        sendData = std::make_shared<std::vector<uint8_t>>();
        recvData = std::make_shared<std::vector<uint8_t>>(recvSize);
        creationTime = Timer::elapsedSeconds();
        //Console::log(Console::VVVVERB, "Msg ID=%i created", id);
    }
    MessageHandle(int id, const std::shared_ptr<std::vector<uint8_t>>& data) : id(id), sendData(data) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1; 
        recvData = std::make_shared<std::vector<uint8_t>>();
        creationTime = Timer::elapsedSeconds();
        //Console::log(Console::VVVVERB, "Msg ID=%i created", id);
    }
    MessageHandle(int id, const std::shared_ptr<std::vector<uint8_t>>& sendData, const std::shared_ptr<std::vector<uint8_t>>& recvData) : 
        id(id), sendData(sendData), recvData(recvData) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1;
        creationTime = Timer::elapsedSeconds();
        //Console::log(Console::VVVVERB, "Msg ID=%i created", id);
    }

    ~MessageHandle() {
        sendData = NULL;
        recvData = NULL;
        //Console::log(Console::VVVVERB, "Msg ID=%i deleted", id);
    }

    bool testSent();
    bool testReceived();
    bool shouldCancel(float elapsedTime);
    void cancel();
};

/**
 * A std::shared_ptr around a MessageHandle instance which captures all relevant information
 * on a specific MPI message.
 */
typedef std::shared_ptr<MessageHandle> MessageHandlePtr;

class MyMpi {

public:
    static int _max_msg_length;
    static bool _monitor_off;

    static void init(int argc, char *argv[]);
    static void beginListening();

    static MessageHandlePtr isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static MessageHandlePtr isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object);
    static MessageHandlePtr irecv(MPI_Comm communicator);
    static MessageHandlePtr irecv(MPI_Comm communicator, int tag);
    static MessageHandlePtr irecv(MPI_Comm communicator, int source, int tag);
    static MessageHandlePtr irecv(MPI_Comm communicator, int source, int tag, int size);
    /*
    static MessageHandlePtr  send(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static MessageHandlePtr  send(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object);
    static MessageHandlePtr  recv(MPI_Comm communicator, int tag, int size);
    static MessageHandlePtr  recv(MPI_Comm communicator, int tag);
    */
    static MPI_Request    ireduce(MPI_Comm communicator, float* contribution, float* result, int rootRank);
    static MPI_Request iallreduce(MPI_Comm communicator, float* contribution, float* result);
    static MPI_Request iallreduce(MPI_Comm communicator, float* contribution, float* result, int numFloats);

    static bool test(MPI_Request& request, MPI_Status& status);

    static std::vector<MessageHandlePtr> poll(float elapsedTime = Timer::elapsedSeconds());
    static int getNumActiveHandles() {
        return _handles.size();
    }
    static bool hasOpenSentHandles();
    static void testSentHandles();
    static bool isAnytimeTag(int tag);

    static int size(MPI_Comm comm);
    static int rank(MPI_Comm comm);
    
    static int nextHandleId();

    // defined in mpi_monitor.*
    static std::string currentCall(double* callStart);

private:
    static std::set<MessageHandlePtr> _handles;
    static std::set<MessageHandlePtr> _sent_handles;
    static std::map<int, MsgTag> _tags;

    static void resetListenerIfNecessary(int tag);

};

#endif


#ifndef DOMPASCH_CUCKOO_BALANCER_BOOSTMPI
#define DOMPASCH_CUCKOO_BALANCER_BOOSTMPI

#include <iostream>
#include <memory>
#include <set>
#include <map>
#include <assert.h>
#include <optional>

// Turn off incompatible function types warning in openmpi
#define OMPI_SKIP_MPICXX 1
#include <mpi.h>

#include "data/serializable.hpp"
#include "util/logger.hpp"
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
    bool selfMessage = false;
    bool finished = false;
    float creationTime = 0;
    MPI_Request request;
    MPI_Status status;
    std::vector<uint8_t> sendData;
    std::vector<uint8_t> recvData;

    MessageHandle() = default;
    MessageHandle(int id, float time = Timer::elapsedSeconds()) : id(id), creationTime(time) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1;
        //log(V5_DEBG, "Msg ID=%i created\n", id);
    }
    MessageHandle(int id, int recvSize, float time = Timer::elapsedSeconds()) : id(id), creationTime(time) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1;
        recvData.resize(recvSize);
        //log(V5_DEBG, "Msg ID=%i created\n", id);
    }
    MessageHandle(int id, const std::vector<uint8_t>& data, float time = Timer::elapsedSeconds()) : 
            id(id), sendData(data), creationTime(time) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1;
        //log(V5_DEBG, "Msg ID=%i created\n", id);
    }
    MessageHandle(int id, const std::vector<uint8_t>& sendData, 
            const std::vector<uint8_t>& recvData, 
            float time = Timer::elapsedSeconds()) : 
            id(id), sendData(sendData), recvData(recvData), creationTime(time) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1;
        //log(V5_DEBG, "Msg ID=%i created\n", id);
    }
    MessageHandle(MessageHandle&& other) : id(other.id), tag(other.tag), source(other.source), 
            sendData(std::move(other.sendData)), recvData(std::move(other.recvData)), selfMessage(other.selfMessage), 
            finished(other.finished), creationTime(other.creationTime), request(other.request), 
            status(other.status) {}

    ~MessageHandle() {
        //log(V5_DEBG, "Msg ID=%i deleted\n", id);
    }

    MessageHandle& operator=(MessageHandle&& other) {
        id = other.id;
        tag = other.tag;
        source = other.source;
        sendData = std::move(other.sendData);
        recvData = std::move(other.recvData);
        selfMessage = other.selfMessage;
        finished = other.finished;
        creationTime = other.creationTime;
        request = other.request;
        status = other.status;
        return *this;
    }

    bool testSent();
    bool testReceived();
    bool shouldCancel(float elapsedTime);
    void cancel();
};

class MyMpi {

public:
    static const int MONKEY_LATENCY = 1;
    static const int MONKEY_DELAY = 2;

    static int _max_msg_length;
    static bool _monitor_off;
    static int _monkey_flags;

    static void init(int argc, char *argv[]);
    static void setOptions(const Parameters& params);
    static void beginListening();

    static void isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static void isend(MPI_Comm communicator, int recvRank, int tag, const std::vector<uint8_t>& object);
    static void irecv(MPI_Comm communicator);
    static void irecv(MPI_Comm communicator, int tag);
    static void irecv(MPI_Comm communicator, int source, int tag);
    static void irecv(MPI_Comm communicator, int source, int tag, int size);
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

    static std::optional<MessageHandle> poll(float elapsedTime = Timer::elapsedSeconds());
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
    static void latencyMonkey();
    static void delayMonkey();

private:
    typedef std::unique_ptr<MessageHandle> MessageHandlePtr;

    static std::vector<MessageHandlePtr> _handles;
    static std::vector<MessageHandlePtr> _sent_handles;
    static robin_hood::unordered_map<int, MsgTag> _tags;

    static void resetListenerIfNecessary(int tag);

};

#endif

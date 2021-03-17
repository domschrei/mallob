
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
#include "util/sys/concurrent_allocator.hpp"

#include "msgtags.h"

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

struct MessageHandle;
typedef std::unique_ptr<MessageHandle> MessageHandlePtr;

class MyMpi {

public:
    static const int MONKEY_LATENCY = 1;
    static const int MONKEY_DELAY = 2;

    static int _max_msg_length;
    static bool _monitor_off;
    static int _monkey_flags;
    struct RecvBundle {int source; int tag; MPI_Comm comm;};
    static ConcurrentAllocator<RecvBundle> _alloc;

    static void init(int argc, char *argv[]);
    static void setOptions(const Parameters& params);
    static void beginListening();

    static int isend(MPI_Comm communicator, int recvRank, int tag, const Serializable& object);
    static int isend(MPI_Comm communicator, int recvRank, int tag, const std::vector<uint8_t>& object);
    static int isend(MPI_Comm communicator, int recvRank, int tag, const std::shared_ptr<std::vector<uint8_t>>& object);
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

    static MessageHandlePtr poll(float elapsedTime = Timer::elapsedSeconds());
    static int getNumActiveHandles() {
        return _handles.size();
    }
    static bool hasOpenSentHandles();
    static void testSentHandles(std::vector<int>* finishedIds = nullptr);
    static bool isAnytimeTag(int tag);

    static int size(MPI_Comm comm);
    static int rank(MPI_Comm comm);
    
    static int nextHandleId();

    // defined in mpi_monitor.*
    static std::string currentCall(double* callStart);
    static void latencyMonkey();
    static void delayMonkey();

private:
    static std::vector<MessageHandlePtr> _handles;
    static std::vector<MessageHandlePtr> _sent_handles;
    static robin_hood::unordered_map<int, MsgTag> _tags;

    static void doIsend(MPI_Comm communicator, int recvRank, int tag);
    static void resetListenerIfNecessary(int tag);
};


/*
Represents a single message that is being sent or received.
*/
struct MessageHandle {

private:
    std::shared_ptr<std::vector<uint8_t>> sendData;
    std::vector<uint8_t> recvData;

public:
    int id;
    int tag;
    int source;
    bool selfMessage = false;
    bool finished = false;
    float creationTime = 0;
    MPI_Request request;
    MPI_Status status;

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
            id(id), sendData(new std::vector<uint8_t>(data)), creationTime(time) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1;
        //log(V5_DEBG, "Msg ID=%i created\n", id);
    }
    MessageHandle(int id, const std::shared_ptr<std::vector<uint8_t>>& data, float time = Timer::elapsedSeconds()) : 
            id(id), sendData(data), creationTime(time) {
        status.MPI_SOURCE = -1; 
        status.MPI_TAG = -1;
        //log(V5_DEBG, "Msg ID=%i created\n", id);
    }

    MessageHandle(MessageHandle& other) = delete;
    MessageHandle(MessageHandle&& other) = delete;
    /*
    : id(other.id), tag(other.tag), source(other.source), 
            sendData(std::move(other.sendData)), recvData(std::move(other.recvData)), selfMessage(other.selfMessage), 
            finished(other.finished), creationTime(other.creationTime), request(other.request), 
            status(other.status) {}
    */

    ~MessageHandle() {
        //log(V5_DEBG, "Msg ID=%i deleted\n", id);
    }

    MessageHandle& operator=(const MessageHandle& other) = delete; 
    MessageHandle& operator=(MessageHandle&& other) = delete; 
    /*
    {
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
    }*/

    const std::vector<uint8_t>& getSendData() const { return *sendData;}
    const std::vector<uint8_t>& getRecvData() const { return recvData;}
    void setReceive(std::vector<uint8_t>&& buf) {recvData = std::move(buf);}
    std::vector<uint8_t>&& moveRecvData() { return std::move(recvData);}

    void appendTagToSendData(int tag) {
        int prevSize = sendData->size();
        sendData->resize(prevSize+sizeof(int));
        memcpy(sendData->data()+prevSize, &tag, sizeof(int));
    }
    void receiveSelfMessage(const std::vector<uint8_t>& recvData, int rank) {
        this->recvData = recvData;
        source = rank;
        selfMessage = true;
    }

    bool testSent();
    bool testReceived();
    bool shouldCancel(float elapsedTime);
    void cancel();

    friend void MyMpi::irecv(MPI_Comm communicator, int source, int tag);
    friend void MyMpi::irecv(MPI_Comm communicator, int source, int tag, int size);
    friend void MyMpi::testSentHandles(std::vector<int>* finishedIds);
};

#endif

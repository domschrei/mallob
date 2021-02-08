#ifndef MSCHICK_DYNAMIC_CUBE_REQUEST_COMMUNICATOR_H
#define MSCHICK_DYNAMIC_CUBE_REQUEST_COMMUNICATOR_H

#include <unordered_set>

#include "cube.hpp"
#include "dynamic_cube_sat_job.hpp"

const int MSG_NEW_DYNAMIC_REQUEST = 4096;
const int MSG_NEW_DYNAMIC_ALL_GOOD = 4097;

const int MSG_NEW_DYNAMIC_FULFILL = 4098;

const int MSG_NEW_DYNAMIC_ROOT_REQUEST = 4099;

const int MSG_NEW_DYNAMIC_SEND_TO_ROOT = 4100;

class NewDynamicCubeCommunicator {
   private:
    // The correspondig job instance
    DynamicCubeSatJob &_job;

    LoggingInterface &_logger;

    // How many cubes should be send to a requesting job instance
    int _cubesPerRequest = 0;

    // If this communicator is requesting
    // May set to true during sendMessageToParent
    // May set to false during handle or release#
    bool _isRequesting = false;

    // Counts how many messages were received since last send
    // Is reset after every sendMessageToParent
    int _messageCounter = 0;

    // Accumulated received requesting node ids
    std::vector<int> _requester;

    void satisfyRequests();

    void sendRequestsToParent();
    void sendAllGoodToParent();

    void broadcastRootRequest();

    void sendCubesToRoot(std::vector<Cube> &cubes);

    void fulfillRequest(int target, std::vector<Cube> &cubes);

    void log_send(int destRank, std::vector<int> &payload, const char *str, ...);
    static std::string payloadToString(std::vector<int> &payload);

   public:
    NewDynamicCubeCommunicator(DynamicCubeSatJob &_job, LoggingInterface &logger, int cubesPerRequest);

    // This function is called in intervals on the leafs
    // This message is also called when a node has accumulated a number of messages equal to the sum of its direct children
    // This message should never be called on a suspended node -> Node has no parent
    void sendMessageToParent();

    void handle(int source, JobMessage &msg);

    // This function is called after node was suspendend. 
    // All cubes are send to the root node
    // All requests are deleted, since they belong to suspended nodes
    void releaseAll();

    static bool isDynamicCubeMessage(int tag);
};

#endif /* MSCHICK_DYNAMIC_CUBE_REQUEST_COMMUNICATOR_H */
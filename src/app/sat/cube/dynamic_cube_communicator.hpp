#ifndef MSCHICK_DYNAMIC_CUBE_COMMUNICATOR_H
#define MSCHICK_DYNAMIC_CUBE_COMMUNICATOR_H

#include "cube.hpp"
#include "dynamic_cube_sat_job.hpp"

const int MSG_DYNAMIC_SEND = 1024;
const int MSG_DYNAMIC_REQUEST = 1025;
const int MSG_DYNAMIC_ALL_GOOD = 1026;

const int MSG_DYNAMIC_FULFILL = 1027;

const int MSG_DYNAMIC_SEND_TO_ROOT = 1028;

class DynamicCubeCommunicator {
   private:
    // The correspondig job instance
    DynamicCubeSatJob &_job;

    LoggingInterface &_logger;

    // How many cubes should be send to a requesting job instance
    int _cubesPerRequest = 0;

    // Counts how many messages were received since last send
    // Is reset after every sendMessageToParent
    int _messageCounter = 0;

    // Accumulated received requesting node ids
    std::vector<int> _requester;

    // Accumulated received cubes
    std::vector<Cube> _received_cubes;

    void sendCubesToParent();
    void sendRequestsToParent();
    void sendAllGoodToParent();

    void sendCubesToRoot(std::vector<Cube> &cubes);

    void fulfillRequest(int target, std::vector<Cube> &cubes);

    void log_send(int destRank, std::vector<int> &payload, const char *str, ...);
    static std::string payloadToString(std::vector<int> &payload);

   public:
    DynamicCubeCommunicator(DynamicCubeSatJob &_job, LoggingInterface &logger, int cubesPerRequest);

    // This function is called in intervals on the leafs
    // This message is also called when a node has accumulated a number of messages equal to the sum of its direct children
    // This message should never be called on a suspended node -> Node has no parent
    void sendMessageToParent();

    void handle(int source, JobMessage &msg);

    // This function is called after node was suspendend. 
    // All cubes are send to the root node
    // All requests are  
    void releaseAll();

    static bool isDynamicCubeMessage(int tag);
};

#endif /* MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H */
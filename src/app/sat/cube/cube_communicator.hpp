#ifndef MSCHICK_CUBE_COMMUNICATOR_H
#define MSCHICK_CUBE_COMMUNICATOR_H

#include <vector>

#include "app/job.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"

const int MSG_REQUEST_CUBES = 835;
const int MSG_SEND_CUBES = 836;
const int MSG_RETURN_FAILED_CUBES = 837;
const int MSG_RECEIVED_FAILED_CUBES = 838;

const int MSG_RETURN_FAILED_AND_REQUEST_CUBES = 839;

const int MSG_REQUEST_FAILED_CUBES = 840;
const int MSG_SEND_FAILED_CUBES = 841;
const int MSG_REDUCE_FAILED_CUBES = 842;
const int MSG_BROADCAST_FAILED_CUBES = 843;

const int MSG_REQUEST_CUBES_FROM_PARENT = 844;

class CubeCommunicator {
   private:
    Job &_job;

    LoggingInterface &_logger;

    void log_send(int destRank, const char *str, ...);
    void log_send(int destRank, std::vector<int> &payload, const char *str, ...);

    static std::string payloadToString(std::vector<int> &payload);

   public:
    CubeCommunicator(Job &job, LoggingInterface &logger) : _job(job), _logger(logger) {};

    // Worker requests cubes from root node
    void requestCubes();

    // Worker requests cubes from parent node
    void requestCubesFromParent();

    // Root node sends cubes to target
    void sendCubes(int target, std::vector<int>& serialized_cubes);

    // Worker returns finished cubes to root node
    void returnFailedCubes(std::vector<int>& serialized_failed_cubes);

    // Root signals that it has received the failed cubes
    void receivedFailedCubes(int target);

    // Worker returns failed cubes and requests new cubes
    void returnFailedAndRequestCubes(std::vector<int>& serialized_failed_cubes);

    // New node requests all known failed cubes
    void requestFailedCubes();

    // Send failed cubes to a certain node
    void sendFailedCubes(int target, std::vector<int>& serialized_failed_cubes);

    void sendCubesToRoot(std::vector<int>& serialized_failed_cubes);
};

#endif /* MSCHICK_CUBE_COMMUNICATOR_H */
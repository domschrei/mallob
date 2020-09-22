#ifndef MSCHICK_CUBE_COMMUNICATOR_H
#define MSCHICK_CUBE_COMMUNICATOR_H

#include <vector>

#include "app/job.hpp"

const int MSG_REQUEST_CUBES = 835;
const int MSG_SEND_CUBES = 836;
const int MSG_RETURN_FAILED_CUBES = 837;
const int MSG_RECEIVED_FAILED_CUBES = 838;

class CubeCommunicator {
   private:
    Job* _job = nullptr;

    bool _initialized = false;

   public:
    CubeCommunicator(Job* job) : _job(job) {
        _initialized = true;
    }

    // Worker requests cubes from root node
    void requestCubes();

    // Root node sends cubes to target
    void sendCubes(int target, std::vector<int>& serialized_cubes);

    // Worker returns finished cubes to root node
    void returnFailedCubes(std::vector<int>& serialized_failed_cubes);

    // Root signals that it has received the failed cubes
    void receivedFailedCubes(int target);
};

#endif /* MSCHICK_CUBE_COMMUNICATOR_H */
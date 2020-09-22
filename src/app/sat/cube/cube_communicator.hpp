#ifndef MSCHICK_CUBE_COMMUNICATOR_H
#define MSCHICK_CUBE_COMMUNICATOR_H

#include <vector>

#include "base_cube_sat_job.hpp"

const int MSG_REQUEST_CUBES = 835;
const int MSG_SEND_CUBES = 836;

class CubeCommunicator {

private:
    BaseCubeSatJob* _job = NULL;

    bool _initialized = false;

public:
    CubeCommunicator(BaseCubeSatJob* job) : _job(job) {
        _initialized = true;
    }

    void requestCubes();

    void sendCubes(int target);
    
    void handle(int source, JobMessage& msg);
};

#endif /* MSCHICK_CUBE_COMMUNICATOR_H */
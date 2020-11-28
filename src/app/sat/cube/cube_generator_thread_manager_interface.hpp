#ifndef MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H
#define MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H

#include <atomic>
#include <vector>

#include "cube.hpp"

class CubeGeneratorThreadManagerInterface {
   public:
    CubeGeneratorThreadManagerInterface() {};

    virtual ~CubeGeneratorThreadManagerInterface() {};

    // Places cubes into cubes, waits internally if no cubes available, leaves cubes empty if interrupted. Also failed cubes are send back.
    virtual void getCube(Cube &cube) = 0;

    // Insert failed assumptions
    virtual void shareC(std::vector<Cube> &failed) {};
};

#endif /* MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H */
#ifndef MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H
#define MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H

#include <atomic>
#include <vector>

#include "cube.hpp"

class CubeSolverThreadManagerInterface {
   public:
    CubeSolverThreadManagerInterface() {};

    virtual ~CubeSolverThreadManagerInterface() {};

    // Places cubes into cubes, waits internally if no cubes available, leaves cubes empty if interrupted. Also failed cubes are send back.
    virtual void shareCubes(std::vector<Cube> &cubes, std::vector<Cube> &failed) = 0;

    // Insert failed assumptions
    virtual void shareFailed(std::vector<Cube> &failed) {};

    // Insert learnt clauses
    virtual void shareLearnt(std::vector<int> &learnt) {};
};

#endif /* MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H */
#ifndef MSCHICK_DYNAMIC_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H
#define MSCHICK_DYNAMIC_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H

#include <atomic>
#include <vector>

#include "cube.hpp"

class DynamicCubeSolverThreadManagerInterface {
   public:
    // Interfaces do not need a constructor but an empty virtual desctructor
    // https://stackoverflow.com/questions/24316700/c-abstract-class-destructor
    virtual ~DynamicCubeSolverThreadManagerInterface(){};

    // Insert free local cubes into cubes, waits internally if none are available
    // If interrupted cubes is left empty
    // Found failed cubes should be put into failed
    virtual void shareCube(std::optional<Cube> &failedAssumptions, std::optional<Cube> &nextCube, int id) = 0;
};

#endif /* MSCHICK_DYNAMIC_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H */
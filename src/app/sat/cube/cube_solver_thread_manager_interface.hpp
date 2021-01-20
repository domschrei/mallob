#ifndef MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H
#define MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H

#include <atomic>
#include <vector>

#include "cube.hpp"

class CubeSolverThreadManagerInterface {
   public:
    // Interfaces do not need a constructor but an empty virtual desctructor
    // https://stackoverflow.com/questions/24316700/c-abstract-class-destructor
    virtual ~CubeSolverThreadManagerInterface(){};

    // Insert free local cubes into cubes, waits internally if none are available
    // If interrupted cubes is left empty
    // Found failed cubes should be put into failed
    virtual void shareCubes(std::optional<Cube> &failedAssumptions, std::optional<Cube> &nextCube) = 0;

    // Send failed cubes to manager
    // Not necessarily implemented
    virtual void shareFailed(std::vector<Cube> &failed){};
};

#endif /* MSCHICK_CUBE_SOLVER_THREAD_MANAGER_INTERFACE_H */
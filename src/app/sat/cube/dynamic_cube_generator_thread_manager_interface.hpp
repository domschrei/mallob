#ifndef MSCHICK_DYNAMIC_CUBE_GENERATOR_THREAD_MANAGER_INTERFACE_H
#define MSCHICK_DYNAMIC_CUBE_GENERATOR_THREAD_MANAGER_INTERFACE_H

#include <atomic>
#include <vector>
#include <optional>

#include "cube.hpp"

class DynamicCubeGeneratorThreadManagerInterface {
   public:
    // Interfaces do not need a constructor but an empty virtual destructor
    // https://stackoverflow.com/questions/24316700/c-abstract-class-destructor
    virtual ~DynamicCubeGeneratorThreadManagerInterface(){};

    // last cube may contain the previously splitted cube
    // split lit contains the generated splitting literal, zero if the last given cube was solved
    // If the last given cube was proven to be unsatisfiable, failed contains the failed assumptions
    // At the end next cube is filled with a new cube that should be split
    // If no cube should be split the method waits internally
    virtual void shareCubeToSplit(std::optional<Cube> &lastCube, int splitLit, std::optional<Cube> &failedAssumptions, std::optional<Cube> &nextCube) = 0;
};

#endif /* MSCHICK_DYNAMIC_CUBE_GENERATOR_THREAD_MANAGER_INTERFACE_H */
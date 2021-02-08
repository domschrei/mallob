#ifndef M_SCHICK_DYNAMIC_CUBES_H
#define M_SCHICK_DYNAMIC_CUBES_H

#include <cassert>
#include <optional>
#include <vector>

#include "cube.hpp"

// This manages cubes in a dynamic cube lib
// Each cube can be split and solved simultaneously

// Currently there are no attempts to split different cubes than those who are already solved
// This is okay since the goal of cube and conquer is to generate work
class DynamicCubes {
   private:
    // The data type that is managed locally
    struct DynamicCube {
        // Cube is deep copied into
        DynamicCube(Cube cube) : cube(cube){};

        // The associated cube
        Cube cube;

        // If this cube is being solved by a solver thread
        bool isSolving = false;

        // If this cube is being split by a generator thread
        bool isSplitting = false;
    };

    std::vector<DynamicCube> _dynamic_cubes;

   public:
    DynamicCubes() = default;

    size_t size() { return _dynamic_cubes.size(); }

    // Insert single cube into dynamic cubes
    void insert(Cube &cube) { _dynamic_cubes.emplace_back(cube); }

    // Insert vector of cubes into dynamic cubes
    void insert(std::vector<Cube> &cubes) {
        for (auto &cube : cubes) _dynamic_cubes.emplace_back(cube);
    }

    // Remove all cubes that contain the given failed assumption
    // This is primarily used to remove a failed cube
    // It has the positive side effect to also prune related cubes
    void prune(Cube &failedAssumptions) {
        std::function<bool(DynamicCube &)> includesFailedPredicate = [&failedAssumptions](DynamicCube &dynamicCube) {
            return dynamicCube.cube.includes(failedAssumptions);
        };

        _dynamic_cubes.erase(std::remove_if(_dynamic_cubes.begin(), _dynamic_cubes.end(), includesFailedPredicate), _dynamic_cubes.end());
    }

    // Sets all cubes as not splitting and not being solved
    // This allows a restart when the cubes cannot be released in time
    void resetAssignment() {
        for (auto &dynamic_cube : _dynamic_cubes) {
            dynamic_cube.isSolving = false;
            dynamic_cube.isSplitting = false;
        }
    }

    // May return a cube that is not being solved
    std::optional<Cube> tryToGetACubeForSolving() {
        DynamicCube *selected_dynamic_cube = nullptr;

        for (auto &dynamic_cube : _dynamic_cubes)

            if (!dynamic_cube.isSolving) {
                if (selected_dynamic_cube != nullptr) {
                    // If a cube was selected and this solvable cube is shorter -> change selection
                    if (dynamic_cube.cube.getPath().size() < selected_dynamic_cube->cube.getPath().size()) {
                        selected_dynamic_cube = &dynamic_cube;
                    }
                } else {
                    // Select first possible cube
                    selected_dynamic_cube = &dynamic_cube;
                }
            }

        if (selected_dynamic_cube != nullptr) {
            // A cube was selected -> set it to solving and return it
            selected_dynamic_cube->isSolving = true;

            return selected_dynamic_cube->cube;
        }

        // No cube solvable
        return std::nullopt;
    }

    // Return whether there is a cube that is not being solved
    bool hasACubeForSolving() {
        for (auto &cube : _dynamic_cubes)
            if (cube.isSolving == false) return true;

        return false;
    }

    // May return the shortest cube that is not splitting
    std::optional<Cube> tryToGetACubeForSplitting() {
        DynamicCube *selected_dynamic_cube = nullptr;

        for (auto &dynamic_cube : _dynamic_cubes)

            if (!dynamic_cube.isSplitting) {
                if (selected_dynamic_cube != nullptr) {
                    // If a cube was selected and this splittable cube is shorter -> change selection
                    if (dynamic_cube.cube.getPath().size() < selected_dynamic_cube->cube.getPath().size()) {
                        selected_dynamic_cube = &dynamic_cube;
                    }
                } else {
                    // Select first possible cube
                    selected_dynamic_cube = &dynamic_cube;
                }
            }

        if (selected_dynamic_cube != nullptr) {
            // A cube was selected -> set it to splitting and return it
            selected_dynamic_cube->isSplitting = true;

            return selected_dynamic_cube->cube;
        }

        // No cube splittable
        return std::nullopt;
    }

    // Return whether there is a cube with that is not splitting
    bool hasACubeForSplitting() {
        for (auto &cube : _dynamic_cubes)
            if (!cube.isSplitting) return true;

        return false;
    }

    // Return whether there is a cube that is splitting
    bool hasSplittingCubes() {
        for (auto &cube : _dynamic_cubes)
            if (cube.isSplitting) return true;

        return false;
    }

    // Returns a bool whether the given split cube was added
    bool handleSplit(Cube &splitCube, int splitLit) {
        // TODO Alter cube to use a set of ints instead of a vector to make this error free
        // The loop may miss the same cube with a different order of the lits
        // This should never happen but who knows
        for (auto &dynamicCube : _dynamic_cubes) {
            if (dynamicCube.cube == splitCube) {
                // Extend original cube and set it to not splitting
                dynamicCube.cube.extend(splitLit);
                dynamicCube.isSplitting = false;

                // TODO Notify the assigned solver thread of this extension

                // Create new cube using inverse splitting literal
                splitCube.extend(-splitLit);
                _dynamic_cubes.emplace_back(splitCube);

                return true;
            }
        }
        return false;
    }

    // Extract a vector with free cubes of size [0, max]
    std::vector<Cube> getFreeCubesForSending(size_t max) {
        std::vector<Cube> free_cubes;

        // Get an iterator at the start of the dynamic cubes
        auto it = _dynamic_cubes.begin();

        while (it != _dynamic_cubes.end() && free_cubes.size() < max) {
            // Check whether the iterator points at a free cube
            if (!(*it).isSolving && !(*it).isSplitting) {
                // Deep copy cube into the free cubes vector
                free_cubes.push_back((*it).cube);

                // Erase extracted cube from dynamic cubes
                // erase() invalidates the iterator, and returns a pointer to the element following the erased element
                it = _dynamic_cubes.erase(it);
            }
            // The iterator pointed to a unfree cube so we need to increment the iterator
            else {
                it++;
            }
        }

        return free_cubes;
    }

    void report(LoggingInterface &logger) {
        int cube_count = 0;
        int solving_count = 0;
        int splitting_count = 0;

        for (DynamicCube &dynamic_cube : _dynamic_cubes) {
            cube_count++;
            if (dynamic_cube.isSolving) solving_count++;
            if (dynamic_cube.isSplitting) splitting_count++;
        }

        logger.log(0, "Dynamic Cubes: There are %i cubes of which %i are solved and %i are split", cube_count, solving_count, splitting_count);
    }
};

#endif /* M_SCHICK_DYNAMIC_CUBES_H */
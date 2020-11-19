#include "cube.hpp"

#include <cassert>

const std::vector<int> Cube::getPath() {
    return _path;
}

void Cube::assign(int node) {
    _assignedTo.push_back(node);
}

size_t Cube::getAssignedCount() {
    return _assignedTo.size();
}

void Cube::fail() {
    _failed = true;
}

bool Cube::hasFailed() {
    return _failed;
}

std::vector<int> serializeCubes(std::vector<Cube> &cubes) {
    std::vector<int> serialized_cubes;

    for (auto cube : cubes) {
        auto path = cube.getPath();
        // Insert path at end
        serialized_cubes.insert(std::end(serialized_cubes), std::begin(path), std::end(path));
        // Insert zero after cube
        serialized_cubes.push_back(0);
    }

    return serialized_cubes;
}

std::vector<Cube> unserializeCubes(std::vector<int> &serialized_cubes) {
    std::vector<Cube> cubes;
    std::vector<int> accumulator;

    for (auto lit : serialized_cubes) {
        if (lit == 0) {
            assert(!accumulator.empty());
            // Add to local cubes
            cubes.emplace_back(accumulator.begin(), accumulator.end());
            // Clear local variable
            accumulator.clear();
        } else {
            accumulator.push_back(lit);
        }
    }

    return cubes;
}

void prune(std::vector<Cube> &cubes, std::vector<Cube> &failed) {
    std::function<bool(Cube &)> includesPredicate = [&failed](Cube &cube) {
        for (Cube &failed_cube : failed)
            if (cube.includes(failed_cube))
                return true;

        return false;
    };

    // Erases all root cubes that include a failed cube
    // Behavior is defined if no root cube matches
    // https://stackoverflow.com/questions/24011627/erasing-using-iterator-from-find-or-remove
    // Function follows Erase-remove idiom https://en.wikipedia.org/wiki/Erase%E2%80%93remove_idiom
    cubes.erase(std::remove_if(cubes.begin(), cubes.end(), includesPredicate), cubes.end());
}

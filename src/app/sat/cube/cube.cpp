#include "cube.hpp"

#include <cassert>
#include <iterator>
#include <sstream>

std::vector<int> Cube::getPath() { return _path; }

// TODO: This is very ugly, may be we refactor cube to use a set internally
// Is the order of literals in a cube important? It defines the guiding path...
bool Cube::includes(Cube &otherCube) {
    std::vector<int> myPath = _path;
    std::sort(myPath.begin(), myPath.end());

    std::vector<int> otherPath = otherCube.getPath();
    std::sort(otherPath.begin(), otherPath.end());

    return std::includes(myPath.begin(), myPath.end(), otherPath.begin(), otherPath.end());
}

bool Cube::includes(std::vector<Cube> &otherCubes) {
    for (auto &otherCube : otherCubes)
        if (includes(otherCube)) return true;

    return false;
}

void Cube::extend(int lit) { _path.push_back(lit); }

std::string Cube::toString() {
    // https://www.geeksforgeeks.org/transform-vector-string/
    if (!_path.empty()) {
        std::ostringstream stringStream;

        stringStream << "{ ";

        // Convert all but the last element to avoid a trailing ","
        std::copy(_path.begin(), _path.end() - 1, std::ostream_iterator<int>(stringStream, ", "));

        // Now add the last element with no delimiter and closing tag
        stringStream << _path.back() << " }";

        return stringStream.str();

    } else {
        return "{ }";
    }
}

std::vector<int> Cube::invert() {
    std::vector<int> inverted;
    for (auto lit : _path) inverted.push_back(-lit);
    return inverted;
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
    std::function<bool(Cube &)> includesPredicate = [&failed](Cube &cube) { return cube.includes(failed); };

    // Erases all root cubes that include a failed cube
    // Behavior is defined if no root cube matches
    // https://stackoverflow.com/questions/24011627/erasing-using-iterator-from-find-or-remove
    // Behavior is defined if cubes is empty
    // https://stackoverflow.com/questions/23761273/stdremove-with-vectorerase-and-undefined-behavior
    // Function follows Erase-remove idiom https://en.wikipedia.org/wiki/Erase%E2%80%93remove_idiom
    cubes.erase(std::remove_if(cubes.begin(), cubes.end(), includesPredicate), cubes.end());
}

// This method remove all from new failed superseeded failed cubes in failed and then adds the new failed cubes
void mergeFailed(std::vector<Cube> &failed, std::vector<Cube> &new_failed) {
    // Prune superseeded failed cubes
    prune(failed, new_failed);

    // Add new failed cubes
    failed.insert(failed.end(), new_failed.begin(), new_failed.end());
}
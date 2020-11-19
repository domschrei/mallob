#ifndef MSCHICK_CUBE_H
#define MSCHICK_CUBE_H

#include <set>
#include <vector>
#include <algorithm>

class Cube {
   private:
    std::vector<int> _path;
    std::vector<int> _assignedTo;

    bool _failed{false};

   public:
    Cube(std::vector<int> path) : _path{path} {};
    Cube(std::vector<int>::iterator begin, std::vector<int>::iterator end) : _path{std::vector<int>{begin, end}} {};
    Cube(std::set<int>::iterator begin, std::set<int>::iterator end) : _path{std::vector<int>{begin, end}} {};

    const std::vector<int> getPath();

    void assign(int node);
    size_t getAssignedCount();

    void fail();
    bool hasFailed();

    // TODO: This is very ugly, may be we refactor cube to use a set internally
    // Is the order of literals in a cube important? It defines the guiding path...
    bool includes(Cube &otherCube) {
        std::vector<int> myPath = _path;
        std::sort(myPath.begin(), myPath.end());

        std::vector<int> otherPath = otherCube.getPath();
        std::sort(otherPath.begin(), otherPath.end());

        return std::includes(myPath.begin(), myPath.end(), otherPath.begin(), otherPath.end());
    }

    bool operator==(const Cube &other) const {
        return this->_path == other._path;
    }
};

std::vector<int> serializeCubes(std::vector<Cube> &cubes);

std::vector<Cube> unserializeCubes(std::vector<int> &serialized_cubes);

// Helper method to prune cubes using given failed cubes
void prune(std::vector<Cube> &cubes, std::vector<Cube> &failed);

#endif /* MSCHICK_CUBE_H */
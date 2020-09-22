#ifndef MSCHICK_CUBE_H
#define MSCHICK_CUBE_H

#include <vector>

class Cube {
   private:
    std::vector<int> _path;
    std::vector<int> _assignedTo;

    bool _failed{false};

   public:
    Cube(std::vector<int> path) : _path{path} {};
    Cube(std::vector<int>::iterator begin, std::vector<int>::iterator end) : _path{std::vector<int>{begin, end}} {};

    const std::vector<int> getPath();

    void assign(int node);
    size_t getAssignedCount();

    void fail();
    bool hasFailed();

    bool operator==(const Cube& other) const {
        return this->_path == other._path;
    }
};

std::vector<int> serializeCubes(std::vector<Cube> &cubes);

std::vector<Cube> unserializeCubes(std::vector<int> &serialized_cubes);

#endif /* MSCHICK_CUBE_H */
#ifndef MSCHICK_CUBE_H
#define MSCHICK_CUBE_H

#include <set>
#include <vector>
#include <algorithm>

class Cube {
   private:
    std::vector<int> _path;

   public:
    Cube() = default;
    Cube(std::vector<int> path) : _path{path} {};
    Cube(std::vector<int>::iterator begin, std::vector<int>::iterator end) : _path{std::vector<int>{begin, end}} {};
    Cube(std::set<int>::iterator begin, std::set<int>::iterator end) : _path{std::vector<int>{begin, end}} {};
    
    // https://stackoverflow.com/a/51864979
    // Cube(const Cube &otherCube) {
    //     _path = otherCube._path;
    // } 

    std::vector<int> getPath();

    void extend(int lit);

    std::vector<int> invert();

    bool includes(Cube &otherCube);

    bool includes(std::vector<Cube> &otherCubes);

    bool operator==(const Cube &other) const {
        return this->_path == other._path;
    }

    std::string toString();
};

std::vector<int> serializeCubes(std::vector<Cube> &cubes);

std::vector<Cube> unserializeCubes(std::vector<int> &serialized_cubes);

// Helper method to prune cubes using given failed cubes
void prune(std::vector<Cube> &cubes, std::vector<Cube> &failed);

// Helper method to merge two vectors of failed cubes
void mergeFailed(std::vector<Cube> &original_failed, std::vector<Cube> &new_failed);

#endif /* MSCHICK_CUBE_H */
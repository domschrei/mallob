#include "util/console.hpp"
#include "app/sat/hordesat/cadical/cadical.hpp"

#include "cube_lib.hpp"

void CubeLib::generateCubes() {
    auto *solver = new CaDiCaL::Solver;

    for (auto lit : _formula) {
        solver->add(lit);
    }
    Console::log(Console::INFO, "Formula added to solver");

    _cubes = solver->generate_cubes(5).cubes;
    Console::log(Console::INFO, "Cubes generated");
}

bool CubeLib::hasCubes() const {
    return !_cubes.empty();
}

std::vector<int> CubeLib::getPreparedCubes() {
    return _cubes.at(0);
}

void CubeLib::digestCubes(std::vector<int> cubes) {
    return this->_cubes.push_back(cubes);
}

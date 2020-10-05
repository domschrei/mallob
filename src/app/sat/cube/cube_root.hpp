#ifndef MSCHICK_CUBE_ROOT_H
#define MSCHICK_CUBE_ROOT_H

#include <vector>

#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "cube.hpp"
#include "cube_communicator.hpp"
#include "util/sys/threading.hpp"

class CubeRoot {
   private:
    std::vector<int> &_formula;

    CubeCommunicator &_cube_comm;

    // Termination flag (no atomic needed)
    SatResult &_result;

    // Depth for cube generation
    int _depth;
    size_t _cubes_per_worker;

    // Root cubes
    std::vector<Cube> _root_cubes;
    Mutex _root_cubes_lock;

    std::vector<Cube> prepareCubes(int target);
    
    void digestFailedCubes(std::vector<Cube> &failedCubes);

   public:
    CubeRoot(std::vector<int> &formula, CubeCommunicator &cube_comm, SatResult &result, int depth, size_t cubes_per_worker);
    
    // Generates cubes
    // Returns true if formula was solved during cube generation
    bool generateCubes();

    void handleMessage(int source, JobMessage &msg);
};

#endif /* MSCHICK_CUBE_ROOT_H */
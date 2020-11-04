#ifndef MSCHICK_CUBE_ROOT_H
#define MSCHICK_CUBE_ROOT_H

#include <atomic>
#include <vector>

#include "app/sat/hordesat/solvers/cadical_interface.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "cube_setup.hpp"
#include "cube.hpp"
#include "cube_communicator.hpp"
#include "util/sys/threading.hpp"

class CubeRoot {
   private:
    CaDiCaL::Solver _solver;

    // Flag that signals if the cube generation was interrupted
    std::atomic_bool _isInterrupted{false};

    std::shared_ptr<std::vector<int>> _formula;
    CubeCommunicator &_cube_comm;
    LoggingInterface &_logger;
    SatResult &_result;

    // Local terminator that encapsulates the _isInterrupted flag
    struct Terminator : public CaDiCaL::Terminator {
        Terminator(std::atomic_bool &isInterrupted) : _isInterrupted(isInterrupted) {}

        bool terminate() override {
            return _isInterrupted.load();
        }

       private:
        std::atomic_bool &_isInterrupted;
    };
    
    Terminator _terminator;

    // Depth for cube generation
    int _depth;
    size_t _cubes_per_worker;

    // Root cubes
    std::vector<Cube> _root_cubes;
    Mutex _root_cubes_lock;

    std::vector<Cube> prepareCubes(int target);

    void digestFailedCubes(std::vector<Cube> &failedCubes);

    void parseStatus(int status);

   public:
    CubeRoot(CubeSetup &setup);
    ~CubeRoot();

    // Generates cubes
    // Returns true if the job should start working
    bool generateCubes();

    void interrupt();

    void handleMessage(int source, JobMessage &msg);
};

#endif /* MSCHICK_CUBE_ROOT_H */

#ifndef MSCHICK_CUBE_LIB_H
#define MSCHICK_CUBE_LIB_H

#include <memory>
#include <vector>

#include "cube_communicator.hpp"
#include "cube_root.hpp"
#include "cube_worker.hpp"

class CubeLib {
   private:
    std::vector<int> _formula;

    std::unique_ptr<CubeRoot> _cube_root;
    std::unique_ptr<CubeWorker> _cube_worker;

    // Termination flag
    SatResult _result = UNKNOWN;

    // Flag that blocks all communication on interruption
    std::atomic_bool _isInterrupted{false};

    bool _isRoot = false;

   public:
    // Worker constructor
    CubeLib(std::vector<int> formula, CubeCommunicator &cube_comm);
    // Root constructor
    CubeLib(std::vector<int> formula, CubeCommunicator &cube_comm, int depth, size_t cubes_per_worker);

    bool wantsToCommunicate();
    void beginCommunication();
    void handleMessage(int source, JobMessage& msg);

    void generateCubes();

    void startWorking();

    // Disables all communication methods
    void interrupt();

    // Makes destructable
    void withdraw();

    SatResult getResult() {
        return _result;
    }
};

#endif /* MSCHICK_CUBE_LIB_H */
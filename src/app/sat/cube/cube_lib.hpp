#ifndef MSCHICK_CUBE_LIB_H
#define MSCHICK_CUBE_LIB_H

#include <memory>
#include <vector>

#include "cube_setup.hpp"
#include "cube_communicator.hpp"
#include "cube_root.hpp"
#include "cube_worker_interface.hpp"

class CubeLib {
   private:
    std::unique_ptr<CubeRoot> _cube_root;
    std::unique_ptr<CubeWorkerInterface> _cube_worker;

    // Flag that blocks all communication on interruption
    std::atomic_bool _isInterrupted{false};

   public:
    CubeLib(CubeSetup &setup);
    // ~CubeLib();

    bool wantsToCommunicate();
    void beginCommunication();
    void handleMessage(int source, JobMessage &msg);

    bool generateCubes();

    void startWorking();

    // Makes worker thread terminate asynchronously 
    // Requires that startWorking was called previously
    // Disables all communication methods
    void interrupt();

    // Joins worker thread
    // Requires that interrupt was called previously
    void withdraw();

    // Suspend all working threads
    // Requires that startWorking was called previously
    void suspend();

    // Resumes all working threads
    // Requires that suspend was called previously
    void resume();
};

#endif /* MSCHICK_CUBE_LIB_H */
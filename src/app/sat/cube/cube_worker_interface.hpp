#ifndef MSCHICK_CUBE_WORKER_INTERFACE_H
#define MSCHICK_CUBE_WORKER_INTERFACE_H

#include <atomic>
#include <vector>

#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "cube_communicator.hpp"
#include "cube_setup.hpp"

class CubeWorkerInterface {
   protected:
    std::shared_ptr<std::vector<int>> _formula;
    CubeCommunicator &_cube_comm;
    LoggingInterface &_logger;
    SatResult &_result;

   public:
    CubeWorkerInterface(CubeSetup &setup) : _formula(setup.formula), _cube_comm(setup.cube_comm), _logger(setup.logger), _result(setup.result) {}
    virtual ~CubeWorkerInterface() {_logger.log(0, "Enter destructor of CubeWorkerInterface");}

    // Starts the worker thread
    virtual void startWorking() = 0;

    // Asynchronously interrupts the worker thread
    virtual void interrupt() = 0;
    // Synchronously join the worker thread
    virtual void join() = 0;

    // Asynchonously suspends the worker thread
    // Messages still need to be received. Otherwise the worker will get into a defective state.
    // TODO: Test this assumption even if the job is currently inactive
    virtual void suspend() = 0;
    // Synchronously resumes the worker thread
    virtual void resume() = 0;

    virtual bool wantsToCommunicate() = 0;
    virtual void beginCommunication() = 0;
    virtual void handleMessage(int source, JobMessage &msg) = 0;
};

#endif /* MSCHICK_CUBE_WORKER_INTERFACE_H */
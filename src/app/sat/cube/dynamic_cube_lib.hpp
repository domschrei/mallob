#ifndef MSCHICK_DYNAMIC_CUBE_LIB_H
#define MSCHICK_DYNAMIC_CUBE_LIB_H

#include "dynamic_cube_generator_thread.hpp"
#include "dynamic_cube_solver_thread.hpp"
#include "dynamic_cube_setup.hpp"
#include "dynamic_cubes.hpp"

class DynamicCubeLib : public DynamicCubeSolverThreadManagerInterface, public DynamicCubeGeneratorThreadManagerInterface {
   private:
    LoggingInterface &_logger;

    // Lifecycle of a DynamicCubeLib
    enum State {
        INACTIVE,
        ACTIVE,
        INTERRUPTING,
    };
    std::atomic<State> _state{INACTIVE};

    // Number of local threads that solve cubes
    size_t _solver_thread_count = 0;

    // Cube solver threads
    // Wrap instances in unique_ptr to work around
    // https://stackoverflow.com/questions/42180500/vector-of-thread-wrapper-with-thread-bound-to-class-member-function
    std::vector<std::unique_ptr<DynamicCubeSolverThread>> _solver_threads;

    size_t _max_dynamic_cubes = 0;

    // Number of local threads that generate cubes
    size_t _generator_thread_count = 0;

    // Cube generator threads
    // Wrap instances in unique_ptr to work around
    // https://stackoverflow.com/questions/42180500/vector-of-thread-wrapper-with-thread-bound-to-class-member-function
    std::vector<std::unique_ptr<DynamicCubeGeneratorThread>> _generator_threads;

    // size of send cubes
    size_t _cubes_per_worker = 0;

    // State to manage cube requests via wantsToCommunicate and beginCommunication
    enum RequestState {
        NONE,
        REQUESTING,
        RECEIVING,
    };
    std::atomic<RequestState> _request_state{NONE};

    // Local cubes
    // May be assigned to a solver
    // May be used for cube generation
    DynamicCubes _dynamic_cubes;

    // Local failed assumptions
    // Received from the local solver threads and possibly generator threads
    // They are stored here in a serialized form until they are shared with the system
    std::vector<int> _local_failed;

    // Lock that guards access to the state, the local cubes, the assigned cubes and the local failed cubes
    Mutex _local_lock;

    // Condition variables that allow solver and generators to wait
    ConditionVariable _solver_cv;
    ConditionVariable _generator_cv;

    // Local learnt clauses
    // Received from the local solver threads
    // They are stored here until they are shared with the system
    // std::vector<int> _local_learnt_clauses;

    // Digest payload of received message

    // Helper methods
    void handleFailedAssumptions(Cube &failed);

   protected:
   public:
    DynamicCubeLib(DynamicCubeSetup &setup, bool isRoot);

    // Activate this lib
    void start();
    // Sends a signal to this lib that it should stop working, normally only used at the end of a job
    void interrupt();
    // Synchronously join an interrupting lib, normally done by a seperate thread at the end of a job
    void join();
    // Synchronously interrupts and joins the lib, normally done when suspending
    void suspend();

    // Interface functions of CubeSolverThreadManagerInterface
    void shareCubes(std::optional<Cube> &failedAssumptions, std::optional<Cube> &nextCube) override;

    // Interface function of CubeGeneratorManagerInterface
    void shareCubeToSplit(std::optional<Cube> &lastCube, int splitLit, std::optional<Cube> &failedAssumptions, std::optional<Cube> &nextCube) override;

    // For cube communication
    bool isRequesting();
    std::vector<Cube> getCubes(size_t bias);
    void digestCubes(std::vector<Cube> &cubes);
    std::vector<Cube> releaseAllCubes();

    // For failed assumption communication
    std::vector<int> getNewFailedAssumptions();
    void digestFailedAssumptions(std::vector<int> &failed_assumptions);
};

#endif /* MSCHICK_DYNAMIC_CUBE_LIB_H */
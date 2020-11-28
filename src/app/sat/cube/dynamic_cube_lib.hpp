#ifndef MSCHICK_DYNAMIC_CUBE_LIB_H
#define MSCHICK_DYNAMIC_CUBE_LIB_H

#include "cube_lib_interface.hpp"
#include "cube_solver_thread.hpp"
#include "cube_generator_thread.hpp"

class DynamicCubeLib : public CubeLibInterface, public CubeSolverThreadManagerInterface {
   private:
    // Number of local threads that solve cubes
    size_t _solver_thread_count = 0;

    // Cube solver threads
    // Wrap instances in unique_ptr to work around
    // https://stackoverflow.com/questions/42180500/vector-of-thread-wrapper-with-thread-bound-to-class-member-function
    std::vector<std::unique_ptr<CubeSolverThread>> _solver_threads;

    // Number of local threads that generate cubes
    size_t _generator_thread_count = 0;

    // Cube generator threads
    // Wrap instances in unique_ptr to work around
    // https://stackoverflow.com/questions/42180500/vector-of-thread-wrapper-with-thread-bound-to-class-member-function
    std::vector<std::unique_ptr<CubeGeneratorThread>> _generator_threads;

    // size of send cubes
    size_t _cubes_per_worker = 0;

    // State to manage cube requests via wantsToCommunicate and beginCommunication
    enum State {
        INIT,
        INIT_REQUESTING,
        INIT_RECEIVING,
        ACTIVE,
        REQUESTING,
        RECEIVING,
        INTERRUPTED
    };
    std::atomic<State> _state{INIT};

    // Flags to check if all initialization messages were received
    std::atomic_bool _received_cubes{false};
    std::atomic_bool _received_failed{false};
    std::atomic_bool _received_learnt{false};

    // Free local cubes
    // May be assigned to a solver or used for cube generation
    std::vector<Cube> _local_cubes;

    // Local cubes that are assigned to a solver
    // May be used for cube generation
    // May be retrieved by parent if this solver suspends
    std::vector<Cube> _assigned_local_cubes;

    // Local failed assumptions
    // They are stored here until they are shared with the system
    std::vector<int> _local_failed_assumptions;

    // All known failed cubes are stored in the cube communicator

    // Local learnt clauses
    // They are stored here until they are shared with all other worker
    std::vector<int> _local_learnt_clauses;

    // All known learnt clauses are stored in the clause communicator

    std::atomic_bool _isInterrupted{false};

   protected:
    // Interface functions of CubeLibInterface
    bool wantsToCommunicateImpl() override;
    void beginCommunicationImpl() override;
    void handleMessageImpl(int source, JobMessage &msg) override;

   public:
    DynamicCubeLib(CubeSetup &setup);

    // Interface functions of CubeLibInterface
    void startWorking() override;
    void interrupt() override;
    void withdraw() override;
    void suspend() override;
    void resume() override;

    // Interface functions of CubeSolverThreadManagerInterface
    void shareCubes(std::vector<Cube> &cubes, std::vector<Cube> &failed) override;
    // void shareFailed(std::vector<Cube> &failed) override;
    // void shareLearnt(std::vector<int> &learnt) override;
};

#endif /* MSCHICK_DYNAMIC_CUBE_LIB_H */
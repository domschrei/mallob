#ifndef MSCHICK_CUBE_GENERATOR_THREAD_H
#define MSCHICK_CUBE_GENERATOR_THREAD_H

#include <atomic>
#include <optional>
#include <thread>

#include "app/sat/hordesat/solvers/cadical.hpp"
#include "dynamic_cube_generator_thread_manager_interface.hpp"
#include "dynamic_cube_setup.hpp"

class DynamicCubeGeneratorThread {
   private:
    DynamicCubeGeneratorThreadManagerInterface &_manager;

    std::shared_ptr<std::vector<int>> _formula;

    LoggingInterface &_logger;

    // May solve formula during lookahead
    SatResult &_result;

    CaDiCaL::Solver _solver;

    // Cube that should be expanded
    // This is optional since it is set from the outside and needs to be possibly empty
    std::optional<Cube> _cube;

    // The found splitting literal
    int _split_literal = 0;

    // The found failed assumptions
    // This is optional since it is optionally filled during generate()
    std::optional<Cube> _failed;

    // Buffer for new failed cubes
    // They are added before every call to lookahead
    std::vector<int> _new_failed_cubes;

    // Mutex that protects access to the new failed cubes
    Mutex _new_failed_cubes_lock;

    std::thread _thread;

    std::atomic_bool _isInterrupted{false};

    // Local terminator that encapsulates the _isInterrupted flag
    struct Terminator : public CaDiCaL::Terminator {
        Terminator(std::atomic_bool &isInterrupted) : _isInterrupted(isInterrupted) {}

        bool terminate() override { return _isInterrupted.load(); }

       private:
        std::atomic_bool &_isInterrupted;
    };

    Terminator _terminator;

    static std::atomic<int> _counter;

    int _instance_counter = 0;

    size_t _added_failed_assumptions_buffer = 0;

    void run();
    void generate();

   public:
    DynamicCubeGeneratorThread(DynamicCubeGeneratorThreadManagerInterface &manager, DynamicCubeSetup &setup);
    ~DynamicCubeGeneratorThread();

    void start();
    void interrupt();
    void join();

    void handleFailed(const std::vector<int> &failed);
};

#endif /* MSCHICK_CUBE_GENERATOR_THREAD_H */
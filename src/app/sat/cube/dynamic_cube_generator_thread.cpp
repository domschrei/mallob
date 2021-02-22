#include "dynamic_cube_generator_thread.hpp"

#include <cassert>

#include "util/console.hpp"

std::atomic<int> DynamicCubeGeneratorThread::_counter{0};

DynamicCubeGeneratorThread::DynamicCubeGeneratorThread(DynamicCubeGeneratorThreadManagerInterface &manager, const DynamicCubeSetup &setup)
    : _manager(manager),
      _formula(setup.formula),
      _logger(setup.logger),
      _result(setup.result),
      _terminator(_isInterrupted),
      _instance_counter{DynamicCubeGeneratorThread::_counter++} {
    // Connect terminator
    _solver.connect_terminator(&_terminator);
    _cube_checker.connect_terminator(&_terminator);

    // Set use cube checker
    _use_cube_checker = setup.params.isSet("check_cube_in_gen");

    // Initialization is done in a seperate thread thus hard work is allowed
    // Also this allows a universal start
    // Read formula
    for (int lit : *_formula.get()) _solver.add(lit);
}

DynamicCubeGeneratorThread::~DynamicCubeGeneratorThread() {
    if (_thread.joinable()) _thread.join();
}

void DynamicCubeGeneratorThread::start() {
    _isInterrupted.store(false);

    assert(!_thread.joinable());

    _thread = std::thread(&DynamicCubeGeneratorThread::run, this);
}

void DynamicCubeGeneratorThread::interrupt() {
    // This interrupts the solver since it is referenced in the connected terminator
    _isInterrupted.store(true);
}

void DynamicCubeGeneratorThread::join() {
    assert(_thread.joinable());

    _thread.join();
}

void DynamicCubeGeneratorThread::run() {
    _logger.log(0, "DynamicCubeGeneratorThread %i: Entering the main loop", _instance_counter);

    while (!_isInterrupted) {
        // Set last cube and reset cube
        std::optional<Cube> lastCube = _cube;
        _cube.reset();

        // Send result and request new cube
        _manager.shareCubeToSplit(lastCube, _split_literal, _failed, _cube, _instance_counter);

        // Failed assumptions were sent
        _failed.reset();

        // Reset split literal
        _split_literal = 0;

        {
            const std::lock_guard<Mutex> lock(_new_failed_cubes_lock);

            if (!_new_failed_cubes.empty()) {
                _logger.log(0, "DynamicCubeGeneratorThread %i: Adding new failed clauses. Buffer size: %zu", _instance_counter, _new_failed_cubes.size());

                // Add received failed cubes to formula
                for (int lit : _new_failed_cubes) _solver.add(lit);

                // Add received failed cubes to cube checker
                if (_use_cube_checker)
                    for (int lit : _new_failed_cubes) _cube_checker.add(lit);

                _added_failed_assumptions_buffer += _new_failed_cubes.size();

                // Reset buffer for received failed cubes
                _new_failed_cubes.clear();
            }
        }

        // Start work
        generate();

        // Exit loop if formula was solved
        if (_result != UNKNOWN) return;
    }
    _logger.log(0, "DynamicCubeGeneratorThread %i: Leaving the main loop", _instance_counter);

    // Deleting the current cube so it will not be set on a resume
    // Otherwise on a call of shareCubeToSplit there would be a last cube and failed would be empty
    _cube.reset();
}

void DynamicCubeGeneratorThread::generate() {
    if (_cube.has_value()) {
        _logger.log(0, "DynamicCubeGeneratorThread %i: Started expanding a cube with size %zu", _instance_counter, _cube.value().getPath().size());

        // If _use_cube_checker is set, check cube before lookahead
        if (_use_cube_checker ? check() : true) {
            auto path = _cube.value().getPath();

            // Solver assumes cube
            for (auto lit : path) _solver.assume(lit);

            // Lookahead
            _split_literal = _solver.lookahead();

            // If lookahead returns 0 the formula is proven to be either SAT or UNSAT
            if (_split_literal == 0) {
                // Return if split literal is 0 due to an interruption
                if (_isInterrupted) {
                    _logger.log(0, "DynamicCubeGeneratorThread %i: Interruption during lookahead", _instance_counter);

                    return;
                }

                // For the formula #30 we saw that the split literal and the status may are zero we therefore try to solve here, this should return quickly
                if (_solver.status() == 0) {
                    _logger.log(0, "DynamicCubeGeneratorThread %i: Split literal and status are zero -> Start solving", _instance_counter);

                    // Reassume cube
                    for (auto lit : path) _solver.assume(lit);

                    // Try solving
                    _solver.solve();

                    _logger.log(0, "DynamicCubeGeneratorThread %i: Finished solving", _instance_counter);
                }

                // Return if status is still 0 due to an interruption while solving
                if (_solver.status() == 0 && _isInterrupted) {
                    _logger.log(0, "DynamicCubeGeneratorThread %i: Interruption during solving", _instance_counter);

                    return;
                }

                assert(_solver.status() == 10 || _solver.status() == 20);

                if (_solver.status() == 10) {
                    _logger.log(0, "DynamicCubeGeneratorThread %i: Found a solution: SAT", _instance_counter);
                    _logger.log(0, "DynamicCubeGeneratorThread %i: Used cube has size %zu", _instance_counter, _cube.value().getPath().size());
                    _logger.log(0, "DynamicCubeGeneratorThread %i: Size of added buffer from failed assumptions: %zu", _instance_counter,
                                _added_failed_assumptions_buffer);
                    _result = SAT;

                } else if (_solver.status() == 20) {
                    // Gather failed assumptions
                    std::vector<int> failed_assumptions;
                    for (auto lit : path)
                        if (_solver.failed(lit)) failed_assumptions.push_back(lit);

                    if (failed_assumptions.empty()) {
                        _logger.log(0, "DynamicCubeGeneratorThread %i: Found a solution: UNSAT", _instance_counter);
                        _logger.log(0, "DynamicCubeGeneratorThread %i: Used cube has size %zu", _instance_counter, _cube.value().getPath().size());
                        _logger.log(0, "DynamicCubeGeneratorThread %i: Size of added buffer from failed assumptions: %zu", _instance_counter,
                                    _added_failed_assumptions_buffer);

                        // Intersection of assumptions and core is empty -> Formula is unsatisfiable
                        _result = UNSAT;

                    } else {
                        _logger.log(0, "DynamicCubeGeneratorThread %i: The cube failed, %zu assumptions failed", _instance_counter, failed_assumptions.size());
                        // At least one assumption failed -> Set failed
                        _failed.emplace(failed_assumptions);
                    }
                }
            }
            _logger.log(0, "DynamicCubeGeneratorThread %i: Found split literal %i", _instance_counter, _split_literal);
        }

    } else {
        _logger.log(0, "DynamicCubeGeneratorThread %i: Skipped generating, because no cube is available", _instance_counter);
    }
}

bool DynamicCubeGeneratorThread::check() {
    assert(_cube.has_value());

    _logger.log(0, "DynamicCubeGeneratorThread %i: Checking a cube with size %zu", _instance_counter, _cube.value().getPath().size());

    auto path = _cube.value().getPath();

    // Cube checker assumes cube
    for (auto lit : path) _cube_checker.assume(lit);

    // Check cube
    auto result = _cube_checker.solve();

    if (result == 0) {
        _logger.log(0, "DynamicCubeGeneratorThread %i: Interruption during cube checking", _instance_counter);

        assert(_isInterrupted);

        return false;

    } else if (result == 10) {
        _logger.log(0, "DynamicCubeGeneratorThread %i: The Cube is valid", _instance_counter);

        return true;

    } else if (result == 20) {
        _logger.log(0, "DynamicCubeGeneratorThread %i: The Cube is conflicting with the failed clauses", _instance_counter);

        // Gather failed assumptions
        std::vector<int> failed_assumptions;
        for (auto lit : path)
            if (_cube_checker.failed(lit)) failed_assumptions.push_back(lit);

        if (failed_assumptions.empty()) {
            _logger.log(0, "DynamicCubeGeneratorThread %i: Found a solution: UNSAT", _instance_counter);
            _logger.log(0, "DynamicCubeGeneratorThread %i: Used cube has size %zu", _instance_counter, _cube.value().getPath().size());
            _logger.log(0, "DynamicCubeGeneratorThread %i: Size of added buffer from failed assumptions: %zu", _instance_counter,
                        _added_failed_assumptions_buffer);

            // The added failed cubes make the formula unsatisfiable
            _result = UNSAT;

        } else {
            _logger.log(0, "DynamicCubeGeneratorThread %i: The cube failed, %zu assumptions failed", _instance_counter, failed_assumptions.size());
            // The cube failed
            _failed.emplace(failed_assumptions);
        }

        return false;
    }

    // TODO abort
    Console::fail("This should never be reached");

    return false;
}

void DynamicCubeGeneratorThread::handleFailed(const std::vector<int> &failed) {
    const std::lock_guard<Mutex> lock(_new_failed_cubes_lock);

    _logger.log(0, "DynamicCubeGeneratorThread %i: Inserting new failed clauses. Buffer size: %zu", _instance_counter, failed.size());

    // Insert failed cubes at the end of new failed cubes
    _new_failed_cubes.insert(_new_failed_cubes.end(), failed.begin(), failed.end());
}

#include "dynamic_cube_generator_thread.hpp"

#include <cassert>

std::atomic<int> DynamicCubeGeneratorThread::_counter{0};

DynamicCubeGeneratorThread::DynamicCubeGeneratorThread(DynamicCubeGeneratorThreadManagerInterface &manager, DynamicCubeSetup &setup)
    : _manager(manager),
      _formula(setup.formula),
      _logger(setup.logger),
      _result(setup.result),
      _terminator(_isInterrupted),
      _instance_counter{DynamicCubeGeneratorThread::_counter++} {
    // Connect terminator
    _solver.connect_terminator(&_terminator);
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
    while (!_isInterrupted) {
        // Set last cube and reset cube
        std::optional<Cube> lastCube = _cube;
        _cube.reset();

        // Send result and request new cube
        _manager.shareCubeToSplit(lastCube, _split_literal, _failed, _cube);

        // Failed assumptions were sent
        _failed.reset();

        {
            const std::lock_guard<Mutex> lock(_new_failed_cubes_lock);

            _logger.log(0, "DynamicCubeGeneratorThread %i: Adding new failed clauses. Buffer size: %zu", _instance_counter, _new_failed_cubes.size());

            // Add received failed cubes to formula
            for (int lit : _new_failed_cubes) _solver.add(lit);

            _added_failed_assumptions_buffer += _new_failed_cubes.size();

            // Reset buffer for received failed cubes
            _new_failed_cubes.clear();
        }

        // Start work
        generate();

        // Exit loop if formula was solved
        if (_result != UNKNOWN) return;
    }
    _logger.log(0, "DynamicCubeGeneratorThread %i: Leaving the main loop", _instance_counter);
}

void DynamicCubeGeneratorThread::generate() {
    if (_cube.has_value()) {
        _logger.log(0, "DynamicCubeGeneratorThread %i: Started expanding the cube %s", _instance_counter, _cube.value().toString().c_str());

        // Assume cube
        auto path = _cube.value().getPath();
        for (auto lit : path) _solver.assume(lit);

        // Lookahead
        _split_literal = _solver.lookahead();

        // If lookahead returns 0 the formula is proven to be either SAT or UNSAT
        if (_split_literal == 0) {
            // Return if split literal is 0 due to an interruption
            if (_isInterrupted) return;

            // For the formula #30 we saw that the split literal and the status may are zero we therefore try to solve here, this should return quickly
            if (_solver.status() == 0) {
                _logger.log(0, "DynamicCubeGeneratorThread %i: Split literal and status are zero -> Start solving", _instance_counter);

                // Reassume cube
                for (auto lit : path) _solver.assume(lit);

                // Try solving
                _solver.solve();

                _logger.log(0, "DynamicCubeGeneratorThread %i: Finished solving", _instance_counter);
            }

            assert(_solver.status() == 10 || _solver.status() == 20);

            if (_solver.status() == 10) {
                _logger.log(0, "DynamicCubeGeneratorThread %i: Found a solution: SAT", _instance_counter);
                _logger.log(0, "DynamicCubeGeneratorThread %i: Used cube %s", _instance_counter, _cube.value().toString().c_str());
                _logger.log(0, "DynamicCubeGeneratorThread %i: Size of added buffer of failed assumption: %zu", _instance_counter, _added_failed_assumptions_buffer);
                _result = SAT;

            } else if (_solver.status() == 20) {
                // Gather failed assumptions
                std::vector<int> failed_assumptions;
                for (auto lit : path)
                    if (_solver.failed(lit)) failed_assumptions.push_back(lit);

                if (failed_assumptions.size() > 0) {
                    // At least one assumption failed -> Set failed
                    _failed.emplace(failed_assumptions);

                } else {
                    _logger.log(0, "DynamicCubeGeneratorThread %i: Found a solution: UNSAT", _instance_counter);
                    _logger.log(0, "DynamicCubeGeneratorThread %i: Used cube %s", _instance_counter, _cube.value().toString().c_str());
                    _logger.log(0, "DynamicCubeGeneratorThread %i: Size of added buffer of failed assumption: %zu", _instance_counter, _added_failed_assumptions_buffer);

                    // Intersection of assumptions and core is empty -> Formula is unsatisfiable
                    _result = UNSAT;
                }
            }
        }
        _logger.log(0, "DynamicCubeGeneratorThread %i: Found split literal %i", _instance_counter, _split_literal);

    } else {
        _logger.log(0, "DynamicCubeGeneratorThread %i: Skipped generating, because no cube is available", _instance_counter);
    }
}

void DynamicCubeGeneratorThread::handleFailed(const std::vector<int> &failed) {
    const std::lock_guard<Mutex> lock(_new_failed_cubes_lock);

    _logger.log(0, "DynamicCubeGeneratorThread %i: Inserting new failed assumption. Buffer size: %zu", _instance_counter, failed.size());

    // Insert failed cubes at the end of new failed cubes
    _new_failed_cubes.insert(_new_failed_cubes.end(), failed.begin(), failed.end());
}

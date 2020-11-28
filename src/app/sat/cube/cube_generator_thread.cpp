#include "cube_generator_thread.hpp"

#include <cassert>

CubeGeneratorThread::CubeSolverThread(CubeGeneratorThreadManagerInterface &worker, CubeSetup &setup) : _worker(worker), _formula(setup.formula), _logger(setup.logger), _result(setup.result) {
    // Initialize solver
    SolverSetup solver_setup;
    solver_setup.logger = &_logger;

    // TODO Fill with valid values
    solver_setup.globalId = 0;
    solver_setup.localId = 0;
    solver_setup.jobname = "cube";
    solver_setup.diversificationIndex = 0;

    _solver = std::make_unique<Cadical>(solver_setup);
}

CubeSolverThread::~CubeSolverThread() {
    if (_thread.joinable())
        _thread.join();
}

void CubeSolverThread::start() {
    // Read formula
    for (int lit : *_formula.get())
        _solver->addLiteral(lit);

    _thread = std::thread(&CubeGeneratorThread::run, this);
}

void CubeSolverThread::interrupt() {
    _isInterrupted.store(true);

    // Interrupt solver
    _solver->interrupt();
}

void CubeSolverThread::suspend() {
    _solver->suspend();
}

void CubeSolverThread::unsuspend() {
    _solver->resume();
}

void CubeSolverThread::run() {
    while (!_isInterrupted) {
        // Request cubes
        _worker.shareCubes(_cubes, _failed_cubes);

        // Failed cubes were sent
        _failed_cubes.clear();

        solve();

        if (_result != UNKNOWN)
            return;

        // All cubes were solved or solver was interrupted
        _cubes.clear();
    }
    _logger.log(0, "Leaving the main loop");
}

void CubeSolverThread::solve() {
    for (Cube &cube : _cubes) {
        _logger.log(0, "Started solving a cube");

        if (includesFailedCube(cube)) {
            _logger.log(0, "Skipped cube");
            continue;
        }

        auto path = cube.getPath();
        auto result = _solver->solve(path);

        // Check result
        if (result == SAT) {
            _logger.log(1, "Found a solution: SAT");
            _result = SAT;

            return;

        } else if (result == UNKNOWN) {
            _logger.log(1, "Solving interrupted");

            return;

        } else if (result == UNSAT) {
            _logger.log(1, "Cube failed");

            auto failed_assumps = _solver->getFailedAssumptions();

            if (failed_assumps.size() > 0) {
                _logger.log(1, "Added failed cube");

                // At least one assumption failed -> Append to _failed_cubes
                _failed_cubes.emplace_back(failed_assumps.begin(), failed_assumps.end());

            } else {
                _logger.log(1, "Found a solution: UNSAT");

                // Intersection of assumptions and core is empty -> Formula is unsatisfiable
                _result = UNSAT;

                return;
            }
        }
    }
    // All cubes were unsatisfiable and always at least one assumption failed or cubes is empty
}

bool CubeSolverThread::includesFailedCube(Cube &cube) {
    for (Cube &failed_cube : _failed_cubes)
        if (cube.includes(failed_cube)) return true;

    return false;
}


#pragma once

#include "app/incsat/inc_sat_controller.hpp"
#include "app/sat/data/definitions.hpp"
#include "core/job_slot_registry.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "interface/api/api_registry.hpp"
#include "mpi.h"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/random.hpp"
#include "util/sys/terminator.hpp"
#include <algorithm>
#include <bitset>
#include <cmath>
#include <memory>
#include <vector>

class CncController {

private:
    const Parameters& _params;
    JobDescription& _desc;
    std::vector<int> _base_formula;

    float _start_time;
    int _status {0};

    enum CubingMode {VAR_OCCURRENCE, LOOKAHEAD_CADICAL} _cubing_mode {LOOKAHEAD_CADICAL};

public:
    CncController(const Parameters& params, JobDescription& desc) : _params(params), _desc(desc) {
        // Extract the base formula to solve.
        _base_formula.insert(_base_formula.end(),
            _desc.getFormulaPayload(0),
            _desc.getFormulaPayload(0)+_desc.getFormulaPayloadSize(0));
        LOG(V2_INFO, "CNC formula: %s\n", StringUtils::getSummary(_base_formula, 20).c_str());

        if (!JobSlotRegistry::isInitialized()) JobSlotRegistry::init(params);
    }

    // Solves the provided SAT formula by means of Cube-and-Conquer.
    JobResult solve() {
        _start_time = Timer::elapsedSeconds();

        // Prepare the dummy "I don't know" job result as a base case.
        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = 0; // unknown

        // Generate a set of cubes
        int depth = 8; // 2^8 = 256 cubes
        LOG(V2_INFO, "CNC generating cubes with depth %i\n", depth);
        std::vector<std::vector<int>> cubes = getCubes(depth);
        ::random_shuffle(cubes.data(), cubes.size()); // shuffle randomly
        int nbGeneratedCubes = cubes.size();
        int nbUnsatCubes = 0; // track number of cubes found UNSAT so far
        LOG(V2_INFO, "CNC generated %i cubes, status=%i\n", nbGeneratedCubes, _status);
        if (_status != 0) {
            res.result = _status;
            LOG(V2_INFO, "CNC CONCLUDE %s\n", _status==10 ? "SAT" : "UNSAT");
            return res;
        }

        // Set up up to four SAT job streams, but no more than the global number of processes
        std::vector<std::unique_ptr<IncSatController>> incsats;
        int numConcStreams = std::min(4, MyMpi::size(MPI_COMM_WORLD));
        for (int i = 0; i < numConcStreams; i++)
            incsats.push_back(addJobStream());

        // Repeatedly loop over all your streams, submitting cubes and fetching results,
        // until a stopping criterion is reached.
        bool stop = false;
        while (!stop) {
            if (nbUnsatCubes == nbGeneratedCubes) {
                // All cubes found UNSAT. We are done!
                LOG(V2_INFO, "CNC CONCLUDE UNSAT\n");
                res.result = UNSAT;
                stop = true;
                break;
            }
            for (auto& incsat : incsats) {
                // already cleaning up this stream?
                if (!incsat->hasStream()) continue;
                auto& stream = incsat->getStream();
                if (stream.finalizing()) continue;
                // result available?
                if (!stream.isIdle() && !stream.isNonblockingSolvePending()) {
                    // -- yes - retrieve it
                    auto [code, witness] = stream.getNonblockingSolveResult();
                    if (code == SAT) {
                        // Cube was found satisfiable! We are done!
                        LOG(V2_INFO, "CNC Cube SAT\n");
                        LOG(V2_INFO, "CNC CONCLUDE SAT\n");
                        res.result = SAT;
                        res.setSolution(std::move(witness));
                        stop = true;
                        break;
                    } else if (code == UNSAT) {
                        // Cube was found unsatisfiable.
                        nbUnsatCubes++;
                        LOG(V2_INFO, "CNC Cube UNSAT (%i/%i)\n", nbUnsatCubes, nbGeneratedCubes);
                    } else {
                        // Cube solving returned UNKNOWN: something has gone wrong
                        // or an internal limit was reached (timeout, interrupt, etc.)
                        stop = true;
                        break;
                    }
                }
                // Is the stream idle right now?
                if (stream.isIdle()) {
                    // Try to submit next cube
                    if (cubes.empty()) {
                        // No cubes left to submit - yield this stream
                        // TODO finalize can sometimes take longer - do concurrently instead?
                        LOG(V3_VERB, "CNC finalizing stream\n");
                        incsat->finalize();
                        LOG(V3_VERB, "CNC stream finalized\n");
                        continue;
                    }
                    // Remove next cube and submit it
                    auto cube = cubes.back(); cubes.pop_back();
                    submitCube(cube, *incsat);
                    assert(!stream.isIdle());
                }
            }
        }

        // RAII should take care of cleaning up all of the remaining job streams
        // and their associated resources.
        incsats.clear();

        return res;
    }

private:
    // Generate a number of cubes exponential in the provided depth.
    std::vector<std::vector<int>> getCubes(int depth) {

        if (_cubing_mode == VAR_OCCURRENCE) {
            std::vector<std::vector<int>> cubes;
            std::vector<int> vars = getSplittingVariables(depth);
            // Just loop over all combinations of (depth) bits
            // and use the splitting variables with according polarities.
            for (size_t i = 0; i < (1<<depth); i++) {
                std::bitset<64> bits(i);
                std::vector<int> cube;
                for (int j = 0; j < depth; j++) {
                    cube.push_back(vars[j] * (bits[j]?1:-1));
                }
                cubes.push_back(std::move(cube));
            }
            return cubes;
        }

        if (_cubing_mode == LOOKAHEAD_CADICAL) {
            SolverSetup setup;
            setup.logger = &Logger::getMainInstance();
            setup.solverType = 'C';
            setup.isJobIncremental = true;
            setup.exportClauses = false;
            std::unique_ptr<Cadical> solver;
            solver.reset(new Cadical(setup));
            solver->setLearnedClauseCallback([&](const Mallob::Clause&, int) {});
            solver->getTerminator().setExternalTerminator([&]() {return false;});
            solver->diversify(0);
            for (int lit : _base_formula) solver->addLiteral(lit);

            std::vector<std::vector<int>> cubes = solver->cube(depth, _status);
            return cubes;
        }

        return {};
    }

    // Select a set to variables to branch over.
    std::vector<int> getSplittingVariables(int depth) {

        // Collect # occurrences of each variable in the formula
        std::vector<std::pair<int, int>> occurrences;
        for (int lit : _base_formula) {
            if (lit == 0) continue;
            int var = std::abs(lit);
            while (var >= occurrences.size())
                occurrences.push_back({occurrences.size(), 0});
            occurrences[var].second++;
        }

        // Sort variables by occurrences in decending order
        struct Compare {
            bool operator()(const std::pair<int, int>& left, const std::pair<int, int>& right) {
                return left.second > right.second;
            }
        };
        std::sort(occurrences.begin(), occurrences.end(), Compare());

        // Return the first (depth) variables
        std::vector<int> vars;
        for (auto item : occurrences) {
            if (item.first == 0) continue;
            LOG(V2_INFO, "CNC var %i : %i occs\n", item.first, item.second);
            vars.push_back(item.first);
            if (vars.size() == depth) break;
        }
        return vars;
    }

    // A bit of boilerplate code to get an incremental SAT solving task in Mallob up and running.
    std::unique_ptr<IncSatController> addJobStream() {

        // Create wrapper object for SAT job stream
        std::unique_ptr<IncSatController> incsat(new IncSatController(_params, APIRegistry::get(), _desc));
        incsat->initInteractiveSolving();
        incsat->getMallobProcessor()->setGroupId("cnc-" + std::to_string(_desc.getId()));
        return incsat;
    }

    // Submit a formula together with the specified cube to the specified (idle!) SatJobStream.
    void submitCube(const std::vector<int>& cube, IncSatController& incsat) {
        std::vector<int> formula;
        if (incsat.getStream().getRevision() == -1) formula = _base_formula;
        incsat.solveNextRevisionNonblocking(std::move(formula), std::vector<int>(cube));
    }

    // Check whether this job should terminate right now.
    bool isTimeoutHit() const {
        if (Terminator::isTerminating())
            return true;
        if (_params.timeLimit() > 0 && Timer::elapsedSeconds() >= _params.timeLimit())
            return true;
        if (_desc.getWallclockLimit() > 0 && (Timer::elapsedSeconds() - _start_time) >= _desc.getWallclockLimit())
            return true;
        return false;
    }
};

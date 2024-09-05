
#pragma once

#include "app/maxsat/maxsat_instance.hpp"
#include "app/maxsat/maxsat_search_procedure.hpp"
#include "app/maxsat/parse/maxsat_reader.hpp"
#include "app/sat/data/definitions.hpp"
#include "app/sat/job/sat_constants.h"
#include "comm/mympi.hpp"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "data/job_transfer.hpp"
#include "interface/api/api_connector.hpp"
#include <algorithm>
#include <climits>
#include <list>
#include <unistd.h>
#include "util/logger.hpp"
// external
#include "rustsat.h"
#include "util/sys/terminator.hpp"
#include "util/params.hpp"

// A MaxSAT solving approach based on solution improving search, i.e., 
// a sequence of SAT calls which impose varying restrictions on admissible
// values of our objective function. This solver is meant to be executed
// within a Mallob *client* process, so that it does not need to take
// a worker process exclusively. The solver submits streams of incremental
// SAT jobs to Mallob.  
class MaxSatSolver {

private:
    const Parameters& _params; // configuration, cmd line arguments
    APIConnector& _api; // for submitting jobs to Mallob
    JobDescription& _desc; // contains our instance to solve and all metadata

    std::unique_ptr<MaxSatInstance> _instance; // the problem instance we're solving

    // holds all active streams of Mallob jobs and allows interacting with them
    std::list<std::unique_ptr<MaxSatSearchProcedure>> _searches;

public:
    // Initializes the solver instance and parses the description's formula.
    MaxSatSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc) {

        LOG(V2_INFO, "Mallob client-side MaxSAT solver, by Jeremias Berg & Dominik Schreiber\n");
        parseFormula();
    }

    // Perform exact MaxSAT solving and return an according result.
    JobResult solve() {

        // Template for the result we will return in the end
        JobResult r;
        r.id = _desc.getId();
        r.revision = 0;
        r.result = RESULT_UNKNOWN;

        // Just for debugging
        _instance->print();

        // Parse the user-provided sequence of search strategies.
        std::string searchStrats = _params.maxSatSearchStrategy();
        const int nbWorkers = _params.numWorkers() == -1 ? MyMpi::size(MPI_COMM_WORLD) : _params.numWorkers();
        // Loop over each specified search strategy
        for (char c : searchStrats) {
            // Check if we have enough workers in the system for another job stream
            if (nbWorkers <= _searches.size()) {
                LOG(V1_WARN, "MAXSAT [WARN] Truncating number of parallel search strategies to %i due to lack of workers\n",
                    nbWorkers);
                break;
            }
            // Initialize search procedure
            _searches.emplace_back(initializeSearchProcedure(c));
        }
        assert(!_searches.empty());

        // Initial SAT call: just solve the hard clauses.
        // We just use the first specified search strategy for this task.
        MaxSatSearchProcedure* search = _searches.front().get();
        // Only for this initial solve call, we don't need to enforce a bound first.
        int resultCode = search->solveBlocking(); // solve and wait for a result
        if (resultCode == RESULT_UNSAT) {
            // UNSAT in the initial call
            LOG(V2_INFO, "MAXSAT Problem is utterly unsatisfiable\n");
            // Return an UNSAT result.
            r.result = RESULT_UNSAT;
            return r;
        }
        if (resultCode != RESULT_SAT) {
            // UNKNOWN or something else - an error in this case since we didn't cancel the job
            LOG(V1_WARN, "[WARN] MAXSAT Unexpected result code %i\n", resultCode);
            return r;
        }
        // Initial formula is SATisfiable.
        LOG(V2_INFO, "MAXSAT Initial model has cost %lu\n", _instance->bestCost);

        // Main loop for solution improving search.
        while (!Terminator::isTerminating() && _instance->lowerBound < _instance->upperBound) {
            // Loop over all search strategies
            for (auto& search : _searches) {
                // No solving procedure ongoing nor pending?
                if (search->isIdle()) {
                    // Compute and enforce the next bound for this strategy
                    search->enforceNextBound();
                    // Launch a SAT job
                    search->solveNonblocking();
                } else if (!search->isNonblockingSolvePending()) {
                    // Current solving procedure has finished:
                    // apply the result to the MaxSAT instance
                    const int resultCode = search->processNonblockingSolveResult();
                }
            }
            // Wait a bit
            usleep(1000 * 10); // 10 ms
        }

        // Did we actually find an optimal result?
        if (_instance->lowerBound >= _instance->upperBound) {
            // construct & return final job result
            r.result = RESULT_OPTIMUM_FOUND;
            r.setSolution(std::move(_instance->bestSolution));
        }
        return r;
    }

private:
    // Parses the formula contained in _desc and initializes _instance accordingly.
    void parseFormula() {

        // Fetch serialized WCNF description
        const int* fPtr = _desc.getFormulaPayload(0);
        const size_t fSize = _desc.getFormulaPayloadSize(0);

        // Traverse the objective function from back to front until you find the beginning
        assert(fSize >= 2 && fPtr[fSize-1] == 0 && fPtr[fSize-2] != 0);
        size_t pos = fSize-2;
        while (pos < fSize && fPtr[pos] != 0) pos--;
        // pos now points at the separation zero right before the objective
        // hard clauses end at the separation zero to the objective
        _instance.reset(new MaxSatInstance(fPtr, pos));

        // Now actually parse the objective function
        ++pos;
        while (pos+1 < fSize) {
            int factor = fPtr[pos];
            int lit = -fPtr[pos+1];
            assert(factor != 0);
            assert(lit != 0);
            _instance->objective.push_back({factor, lit});
            pos += 2;
        }
        // Sort the objective terms by weight in increasing order
        // (may help to find the required steps to take in solution-improving search)
        std::sort(_instance->objective.begin(), _instance->objective.end(),
            [&](const MaxSatInstance::ObjectiveTerm& termLeft, const MaxSatInstance::ObjectiveTerm& termRight) {
            return termLeft.factor < termRight.factor;
        });

        // Extract number of variables
        std::string nbVarsString = _desc.getAppConfiguration().map.at("__NV");
        while (nbVarsString[nbVarsString.size()-1] == '.') 
			nbVarsString.resize(nbVarsString.size()-1);
		_instance->nbVars = atoi(nbVarsString.c_str());

        _instance->lowerBound = 0;
        _instance->upperBound = 0;
        for (auto term : _instance->objective) _instance->upperBound += term.factor;
        _instance->bestCost = ULONG_MAX;
    }

    MaxSatSearchProcedure* initializeSearchProcedure(char c) {
        // Parse search strategy
        MaxSatSearchProcedure::SearchStrategy strat;
        std::string label;
        switch (c) {
        case 'd':
            strat = MaxSatSearchProcedure::DECREASING;
            label = "DEC";
            break;
        case 'i':
            strat = MaxSatSearchProcedure::INCREASING;
            label = "INC";
            break;
        case 'b':
            strat = MaxSatSearchProcedure::BISECTION;
            label = "BIS";
            break;
        case 'r':
            strat = MaxSatSearchProcedure::NAIVE_REFINEMENT;
            label = "NRE";
            break;
        }
        // Initialize search procedure
        return new MaxSatSearchProcedure(_params, _api, _desc,
            *_instance, strat, label);
    }
};

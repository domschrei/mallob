
#pragma once

#include "app/maxsat/parse/maxsat_reader.hpp"
#include "app/maxsat/sat_job_stream.hpp"
#include "app/sat/data/definitions.hpp"
#include "app/sat/job/sat_constants.h"
#include "data/job_description.hpp"
#include "data/job_result.hpp"
#include "data/job_transfer.hpp"
#include "interface/api/api_connector.hpp"
#include <algorithm>
#include <list>
#include <unistd.h>
#include "util/logger.hpp"
// external
#include "rustsat.h"

// C-style clause collector function for RustSAT C API
void maxsat_collect_clause(int lit, void* solver);
// C-style assumption collector function for RustSAT C API
void maxsat_collect_assumption(int lit, void* solver);

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

    // required for Mallob's job submission API
    std::string _username;

    // raw C array of the formula (hard clauses) to solve
    const int* _f_data;
    struct ObjectiveTerm {
        int factor;
        int lit;
    };
    // objective function as a linear combination of literals
    std::vector<ObjectiveTerm> _objective;
    // size of the raw C array _f_data
    const size_t _f_size;
    // number of variables in the formula - update when adding new ones
    unsigned int _nb_vars;

    // vector of 0-separated (hard) clauses to add in the next SAT call
    std::vector<int> _lits_to_add;
    // vector of assumption literals for the next SAT call
    std::vector<int> _assumptions_to_set;

    // holds all active streams of Mallob jobs and allows interacting with them
    std::list<std::unique_ptr<SatJobStream>> _job_streams;

    // the best found satisfying assignment so far
    std::vector<int> _best_solution;
    // the cost associated with the best found satisfying assignment so far
    size_t _best_cost;

public:
    // Initializes the solver instance and parses the description's formula.
    MaxSatSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc), _username("maxsat#" + std::to_string(_desc.getId())),
        _f_data(desc.getFormulaPayload(0)), _f_size([&]() {
            return parseFormulaAndGetFormulaSize();
        }()) {
        LOG(V2_INFO, "Mallob client-side MaxSAT solver, by Jeremias Berg & Dominik Schreiber\n");
        std::string nbVarsString = desc.getAppConfiguration().map.at("__NV");
        while (nbVarsString[nbVarsString.size()-1] == '.') 
			nbVarsString.resize(nbVarsString.size()-1);
		_nb_vars = atoi(nbVarsString.c_str());
    }

    // Perform exact MaxSAT solving and return an according result.
    JobResult solve() {

        // Template for the result we will return in the end
        JobResult r;
        r.id = _desc.getId();
        r.revision = 0;
        r.result = RESULT_UNKNOWN;

        // Just for debugging
        printFormula();

        // Little bit of boilerplate to initialize a new incremental SAT job stream
        _job_streams.emplace_back(new SatJobStream(_params, _api, _desc,
            _job_streams.size(), true));
        SatJobStream* stream = _job_streams.back().get();

        // Initial SAT call: just solve the hard clauses.
        std::vector<int> initialFormula(_f_data, _f_data+_f_size);
        stream->submitNext(std::move(initialFormula), {});
        // TODO Once we have parallel jobs, we may want to think about a more clever way
        // to wait for a job to finish, like with condition variables.
        while (stream->isPending()) usleep(1000 * 10); // 10 ms

        // Job is done - retrieve the result.
        auto result = std::move(stream->getResult());
        const int resultCode = result["result"]["resultcode"];
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

        // Retrieve the initial model and compute its cost as a first upper bound.
        _best_solution = std::move(result["result"]["solution"].get<std::vector<int>>());
        _best_cost = getObjectiveValueOfModel(_best_solution);
        LOG(V2_INFO, "MAXSAT Initial model has cost %lu\n", _best_cost);
        const size_t _initial_cost = _best_cost;

        // Initialize cardinality constraint encoder.
        auto cardi = RustSAT::gte_new();
        for (auto& [factor, lit] : _objective) {
            // add each term of the objective function
            RustSAT::gte_add(cardi, lit, factor);
        }

        // We can now submit a sequence of increments of this job to Mallob.
        // Begin with trivial initial bounds. TODO Better initial bounds?
        size_t lb = 0, ub = _best_cost;
        while (lb != ub) {
            // Encode any cardinality constraints that are still missing for the upcoming call
            RustSAT::gte_encode_ub(cardi, ub-1, _initial_cost-1, &_nb_vars, &maxsat_collect_clause, this);
            // Generate the assumptions needed for this particular upper bound
            RustSAT::gte_enforce_ub(cardi, ub-1, &maxsat_collect_assumption, this);

            // Submit a SAT job increment and wait until it returns
            LOG(V2_INFO, "MAXSAT testing bound %lu\n", ub-1);
            stream->submitNext(std::move(_lits_to_add), std::move(_assumptions_to_set));
            while (stream->isPending()) usleep(1000 * 10); // 10 ms

            // Retrieve the result
            auto result = std::move(stream->getResult());
            const int resultCode = result["result"]["resultcode"];
            if (resultCode == RESULT_UNSAT) {
                // UNSAT
                LOG(V2_INFO, "MAXSAT Bound %lu found unsatisfiable\n", ub-1);
                LOG(V2_INFO, "MAXSAT OPTIMAL COST %lu\n", ub);
                lb = ub;
            } else if (resultCode == RESULT_SAT) {
                // SAT
                std::vector<int> solution = std::move(result["result"]["solution"]);
                const size_t newCost = getObjectiveValueOfModel(solution);
                LOG(V2_INFO, "MAXSAT Found model with cost %lu <= %lu\n", newCost, ub-1);
                assert(newCost < ub);
                ub = newCost;
                _best_solution = std::move(solution);
                _best_cost = newCost;
            } else {
                // UNKNOWN or something else - a problem in this case since we didn't cancel the job
                LOG(V1_WARN, "[WARN] MAXSAT Unexpected result code %i\n", resultCode);
                return r;
            }
        }

        // Clean up cardinality encoder
        RustSAT::gte_drop(cardi);

        // construct & return final job result
        r.result = RESULT_OPTIMUM_FOUND;
        r.setSolution(std::move(_best_solution));
        return r;
    }

private:

    // Parses the formula contained in _desc and returns
    // the total size of the description *without* the objective function.
    int parseFormulaAndGetFormulaSize() {

        // Fetch serialized WCNF description
        const int* fPtr = _desc.getFormulaPayload(0);
        const size_t fSize = _desc.getFormulaPayloadSize(0);

        // Traverse the objective function from back to front until you find the beginning
        assert(fSize >= 2 && fPtr[fSize-1] == 0 && fPtr[fSize-2] != 0);
        size_t pos = fSize-2;
        while (pos < fSize && fPtr[pos] != 0) pos--;
        // pos now points at the separation zero right before the objective
        const size_t endOfClauses = pos; // hard clauses end at the separation zero to the objective 

        // Now actually parse the objective function
        ++pos;
        while (pos+1 < fSize) {
            int factor = fPtr[pos];
            int lit = -fPtr[pos+1];
            assert(factor != 0);
            assert(lit != 0);
            _objective.push_back({factor, lit});
            pos += 2;
        }

        return endOfClauses;
    }

    // Print some nice-to-know diagnostics.
    void printFormula() const {
        LOG(V2_INFO, "%i (hard) clause lits, %i objective terms\n", _f_size, _objective.size());
        std::string o;
        for (auto& term : _objective) {
            o += std::to_string(term.factor) + "*[" + std::to_string(term.lit) + "] + ";
        }
        o = o.substr(0, o.size()-2);
        LOG(V2_INFO, "objective: %s\n", o.c_str());
    }

    // Evaluate a satisfying assignment (as returned by a Mallob SAT job)
    // w.r.t. its objective function cost.
    size_t getObjectiveValueOfModel(const std::vector<int>& model) const {
        size_t sum = 0;
        for (auto& term : _objective) {
            const int termLit = term.lit;
            assert(std::abs(termLit) < model.size());
            const int modelLit = model[std::abs(termLit)];
            assert(termLit == modelLit || termLit == -modelLit);
            if (modelLit == termLit) {
                sum += term.factor;
            }
        }
        return sum;
    }

    // Add a permanent literal to the next SAT call. (0 = end of clause)
    void appendLiteral(int lit) {
        _lits_to_add.push_back(lit);
    }
    // Append an assumption for the next SAT call.
    void appendAssumption(int lit) {
        _assumptions_to_set.push_back(lit);
    }

    friend void maxsat_collect_clause(int lit, void *solver);
    friend void maxsat_collect_assumption(int lit, void *solver);
};

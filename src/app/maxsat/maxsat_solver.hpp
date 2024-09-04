
#pragma once

#include "app/maxsat/parse/maxsat_reader.hpp"
#include "app/maxsat/sat_job_stream.hpp"
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

void maxsat_collect_clause(int lit, void* solver);
void maxsat_collect_assumption(int lit, void* solver);

class MaxSatSolver {

private:
    const Parameters& _params;
    APIConnector& _api;
    JobDescription& _desc;

    std::string _username;

    struct ObjectiveTerm {
        int factor;
        int lit;
    };

    const int* _f_data;
    std::vector<ObjectiveTerm> _objective;
    size_t _sum_of_factors {0};
    const size_t _f_size;
    unsigned int _nb_vars;

    std::vector<int> _lits_to_add;
    std::vector<int> _assumptions_to_set;

    std::list<std::unique_ptr<SatJobStream>> _job_streams;

    std::vector<int> _best_solution;
    size_t _best_cost;

public:
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

    JobResult solve() {

        // Just for debugging
        printFormula();

        // Little bit of boilerplate to initialize a new incremental SAT job stream
        // (We could and should eventually use multiple such streams at once.)
        _job_streams.emplace_back(new SatJobStream(_params, _api, _desc,
            _job_streams.size(), true));
        auto stream = _job_streams.back().get();

        // Initial SAT call: just solve the hard clauses.
        std::vector<int> initialFormula(_f_data, _f_data+_f_size);
        stream->submitNext(std::move(initialFormula), {});
        while (stream->isPending()) usleep(1000 * 10); // 10 ms

        // Retrieve the result
        auto result = std::move(stream->getResult());
        const int resultCode = result["result"]["resultcode"];
        if (resultCode == 20) {
            // UNSAT - this instance is completely unsatisfiable.
            LOG(V2_INFO, "MAXSAT Problem is utterly unsatisfiable\n");
            JobResult r;
            r.id = _desc.getId();
            r.revision = 0;
            r.result = 20;
            return r;
        }
        if (resultCode != 10) {
            // UNKNOWN or something else - an error in this case since we didn't cancel the job
            abort();
        }
        // SAT
        _best_solution = std::move(result["result"]["solution"].get<std::vector<int>>());
        _best_cost = getObjectiveValueOfModel(_best_solution);
        LOG(V2_INFO, "MAXSAT Initial model has cost %lu\n", _best_cost);
        const size_t _initial_cost = _best_cost;

        // Cardinality constraint encoder
        auto cardi = RustSAT::gte_new();
        for (auto& [factor, lit] : _objective) {
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
            if (resultCode == 20) {
                // UNSAT
                LOG(V2_INFO, "MAXSAT Bound %lu found unsatisfiable\n", ub-1);
                LOG(V2_INFO, "MAXSAT OPTIMAL COST %lu\n", ub);
                lb = ub;
            } else if (resultCode == 10) {
                // SAT
                std::vector<int> solution = std::move(result["result"]["solution"]);
                const size_t newCost = getObjectiveValueOfModel(solution);
                LOG(V2_INFO, "MAXSAT Found model with cost %lu <= %lu\n", newCost, ub-1);
                assert(newCost < ub);
                ub = newCost;
                _best_solution = std::move(solution);
                _best_cost = newCost;
            } else {
                // UNKNOWN or something else - an error in this case since we didn't cancel the job
                abort();
            }
        }

        // Clean up cardinality encoder
        RustSAT::gte_drop(cardi);

        // construct proper job result
        JobResult res;
        res.id = _desc.getId();
        res.revision = 0;
        res.result = 30;
        res.setSolution(std::move(_best_solution));
        return res;
    }

private:

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
            _sum_of_factors += factor;
            pos += 2;
        }

        return endOfClauses;
    }

    void printFormula() {

        /*
        // Fetch serialized WCNF description
        const int* fPtr = _desc.getFormulaPayload(0);
        const size_t fSize = _desc.getFormulaPayloadSize(0);

        // Parse the serialized WCNF description into something we can understand and manipulate.
        std::vector<std::vector<int>> hardClauses;
        std::vector<std::pair<int, int>> objective;
        bool readingObjective {false};
        std::vector<int> clause;
        int softUnitWeight {0};
        for (size_t i = 0; i < fSize; i++) {
            int lit = fPtr[i];
            if (lit == 0) {
                if (clause.empty()) {
                    if (readingObjective) {
                        // we are done!
                        break;
                    } else {
                        // we transition to the objective function, i.e., the soft unit clauses
                        readingObjective = true;
                    }
                } else {
                    // a clause is done
                    hardClauses.push_back(std::move(clause));
                }
            } else if (readingObjective) {
                if (softUnitWeight == 0) {
                    // read a soft unit's weight
                    softUnitWeight = lit;
                } else {
                    // read a soft unit's literal and store it
                    objective.push_back({softUnitWeight, lit});
                    softUnitWeight = 0;
                }
            } else {
                // add literal to current clause
                clause.push_back(lit);
            }
        }
        */

        LOG(V2_INFO, "%i (hard) clause lits, %i objective terms\n", _f_size, _objective.size());
        std::string o;
        for (auto& term : _objective) {
            o += std::to_string(term.factor) + "*[" + std::to_string(term.lit) + "] + ";
        }
        o = o.substr(0, o.size()-2);
        LOG(V2_INFO, "objective: %s\n", o.c_str());
    }

    size_t getObjectiveValueOfModel(const std::vector<int>& model) {
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

    void appendLiteral(int lit) {
        _lits_to_add.push_back(lit);
    }
    void appendAssumption(int lit) {
        _assumptions_to_set.push_back(lit);
    }

    friend void maxsat_collect_clause(int lit, void *solver);
    friend void maxsat_collect_assumption(int lit, void *solver);
};


#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <valarray>
#include <vector>

#include "app/maxsat/interval_search.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"

struct MaxSatInstance {

    // raw C array of the formula (hard clauses) to solve
    const int* formulaData;
    struct ObjectiveTerm {
        size_t factor;
        int lit;
    };
    int preprocessLayer {0};
    // objective function as a linear combination of literals
    std::vector<ObjectiveTerm> objective;
    // size of the raw C array _f_data
    size_t formulaSize;
    // number of variables in the formula - update when adding new ones
    unsigned int nbVars;
    size_t sumOfWeights;
    size_t nbUniqueWeights;

    size_t lowerBound;
    size_t upperBound;

    // the best found satisfying assignment so far
    std::vector<int> bestSolution;
    int bestSolutionPreprocessLayer = -1;
    // the cost associated with the best found satisfying assignment so far
    size_t bestCost;

    std::unique_ptr<IntervalSearch> intervalSearch;

    MaxSatInstance(const int* formulaData, size_t formulaSize) : formulaData(formulaData), formulaSize(formulaSize) {}

    // Print some nice-to-know diagnostics.
    void print(int updateLayer = 0) const {
        LOG(V2_INFO, "MAXSAT instance layer=%i lits=%lu #o=%lu sow=%lu #uniq=%lu lb=%lu ub=%lu\n",
            updateLayer, formulaSize, objective.size(), sumOfWeights, nbUniqueWeights, lowerBound, upperBound);
        std::string o;
        for (size_t i = 0; i < objective.size(); i++) {

            // only print head and tail of the objective
            if (i == 5 && objective.size() > 10) o += "... + ";
            if (i >= 5 && i+5 < objective.size()) continue;

            auto& term = objective[i];
            o += std::to_string(term.factor) + "*[" + std::to_string(term.lit) + "] + ";
        }
        o = o.substr(0, o.size()-2);
        LOG(V2_INFO, "MAXSAT objective: %s\n", o.c_str());
    }

    // Evaluate a satisfying assignment (as returned by a Mallob SAT job)
    // w.r.t. its objective function cost.
    size_t getCostOfModel(const std::vector<int>& model) const {
        size_t sum = 0;
        for (auto& term : objective) {
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

    // Returns some number x strictly greater than cost. The guarantee is that
    // any "skipped" values between x and cost are not reachable as cost values
    // of the objective function.
    size_t findNextPossibleHigherCost(size_t cost) const {
        return cost+1; // trivial
    }
    // Returns some number x strictly less than cost. The guarantee is that
    // any "skipped" values between x and cost are not reachable as cost values
    // of the objective function.
    size_t findNextPossibleLowerCost(size_t cost) const {
        return cost-1; // trivial
    }
};


#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <valarray>
#include <vector>

#include "app/maxsat/interval_search.hpp"
#include "data/job_description.hpp"
#include "robin_set.h"
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
    MaxSatInstance(JobDescription& _desc, bool fromMaxPre, float intervalSkew) {

        // Fetch serialized WCNF description
        const int* fPtr = _desc.getFormulaPayload(0);
        const size_t fSize = _desc.getFormulaPayloadSize(0);

        // Traverse the objective function from back to front until you find the beginning
        assert(fSize >= 1);
        size_t pos = fSize;
        pos -= 2;
        unsigned long ub = * (unsigned long*) (fPtr + pos);
        pos -= 2;
        unsigned long lb = * (unsigned long*) (fPtr + pos);
        pos--;
        const int nbObjectiveTerms = fPtr[pos];
        pos -= 3*nbObjectiveTerms;
        LOG(V2_INFO, "MAXSAT lb=%lu ub=%lu o=%i\n", lb, ub, nbObjectiveTerms);
        Logger::getMainInstance().flush();
        assert(nbObjectiveTerms >= 0);
        pos--;
        assert(fPtr[pos] == 0);
        // pos now points at the separation zero right before the objective
        // hard clauses end at the separation zero to the objective
        formulaData = fPtr;
        formulaSize = pos;
        lowerBound = lb;
        upperBound = ub;

        // Now actually parse the objective function
        ++pos;
        tsl::robin_set<size_t> uniqueFactors;
        while (objective.size() < nbObjectiveTerms) {
            size_t factor = * (size_t*) (fPtr+pos);
            // MaxPRE already flips the literals' polarity
#if MALLOB_USE_MAXPRE == 1
            int lit = (fromMaxPre ? 1 : -1) * fPtr[pos+2];
#else
            int lit = -1 * fPtr[pos+2];
#endif
            assert(factor != 0);
            assert(lit != 0);
            objective.push_back({factor, lit});
            uniqueFactors.insert(factor);
            pos += 3;
        }
        nbUniqueWeights = uniqueFactors.size();
        // Sort the objective terms by weight in increasing order
        // (may help to find the required steps to take in solution-improving search)
        std::sort(objective.begin(), objective.end(),
            [&](const MaxSatInstance::ObjectiveTerm& termLeft, const MaxSatInstance::ObjectiveTerm& termRight) {
            return termLeft.factor < termRight.factor;
        });

        nbVars = _desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
        sumOfWeights = 0;
        for (auto term : objective) sumOfWeights += term.factor;
        upperBound = std::min(upperBound, sumOfWeights);
        bestCost = ULONG_MAX;

        intervalSearch.reset(new IntervalSearch(intervalSkew));
    }

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


#pragma once

#include "app/sat/execution/solver_setup.hpp"
#include "util/distribution.hpp"
#include "util/logger.hpp"

class ReduceDiversifier {

private:
    const SolverSetup& _setup;
    Logger& _logger;
    std::function<bool(int)> _set_lower;
    std::function<bool(int)> _set_upper;

public:
    ReduceDiversifier(const SolverSetup& setup, Logger& logger,
            std::function<bool(int)> setLower, std::function<bool(int)> setUpper) :
        _setup(setup), _logger(logger), _set_lower(setLower), _set_upper(setUpper) {}

    void apply(int seed) {
        std::mt19937 rng(seed);
        Distribution distribution(rng);

        //Give each Kissat solver a randomized reduce-range, such that some solvers keep most clauses and other solvers other kick most clauses
        int reduce_low = 500;
        int reduce_high = 900;

        assert(_setup.diversifyReduce > 0);
        assert(_setup.diversifyReduce <= 4);

        // Reduce==1: Uniform distribution of range values
        if (_setup.diversifyReduce == 1) {
            distribution.configure(Distribution::UNIFORM, std::vector<double>{
                    /*min=*/(double)(_setup.reduceMin - _setup.reduceDelta), /*max=*/(double) (_setup.reduceMax + _setup.reduceMax)
            });
            int reduce_center = (int) std::round(distribution.sample());
            reduce_low  = std::max(0, reduce_center - _setup.reduceDelta);
            reduce_high = std::min(1000, reduce_center + _setup.reduceDelta);
            LOGGER(_logger, V3_VERB, "Given diversifyReduce=%i, reduceMin=%i, reduceMax=%i, reduceDelta=%i\n", _setup.diversifyReduce, _setup.reduceMin, _setup.reduceMax, _setup.reduceDelta);
            LOGGER(_logger, V3_VERB, "Sampled reduce_center=%i\n", reduce_center);
        }
        // Reduce==2: More extreme range values, 80% of solvers are either full-keep or full-kick, only 20% of solvers are moderate with keep half
        else if (_setup.diversifyReduce == 2) {
            distribution.configure(Distribution::UNIFORM, std::vector<double>{0,1});
            double random_selector = distribution.sample();
            if      (random_selector<0.4) reduce_low = reduce_high = 0;
            else if (random_selector<0.6) reduce_low = reduce_high = 500;
            else                          reduce_low = reduce_high = 1000;
            LOGGER(_logger, V3_VERB, "Given diversifyReduce=%i, reduceDelta=%i\n", _setup.diversifyReduce, _setup.reduceDelta);
        }
        // Reduce==3: Gaussian Distribution
        else if (_setup.diversifyReduce == 3) {
            distribution.configure(Distribution::NORMAL, std::vector<double>{
                /*mean=*/(double)_setup.reduceMean, /*stddev=*/(double)_setup.reduceStddev, /*min=*/(double)_setup.reduceMin, /*max=*/(double)_setup.reduceMax
            });
            int reduce_fixed = (int) std::round(distribution.sample());
            reduce_low  = reduce_fixed;
            reduce_high = reduce_fixed;
            LOGGER(_logger, V3_VERB, "Given diversifyReduce=%i, reduceMin=%i, reduceMax=%i, reduceMean=%i, reduceStddev=%i \n",
                _setup.diversifyReduce, _setup.reduceMin, _setup.reduceMax, _setup.reduceMean, _setup.reduceStddev);
        }
        // Reduce==4: 10% of solvers are either almost full-keep or almost full-kick, 
        // the remaining solvers are slightly perturbed
        else if (_setup.diversifyReduce == 4) {
            distribution.configure(Distribution::UNIFORM, std::vector<double>{0,1});
            if (distribution.sample() < 0.1) {
                // 20% chance: keep or remove almost all clauses (coin flip)
                reduce_low = (distribution.sample() < 0.5) ? 50 : 950;
                reduce_high = reduce_low;
            } else {
                // 80% chance: just add some minor perturbation to the reduce params
                distribution.configure(Distribution::NORMAL, {
                    /*mean=*/0, /*stddev=*/5, /*min=*/-50, /*max=*/50
                });
                reduce_low += distribution.sample();
                reduce_high += distribution.sample();
            }
        }

        bool ok;
        ok = _set_lower(reduce_low);
        if (!ok) abort();
        ok = _set_upper(reduce_high);
        if (!ok) abort();
        LOGGER(_logger, V3_VERB, "reducelo=%i reducehi=%i\n", reduce_low, reduce_high);
    }
};

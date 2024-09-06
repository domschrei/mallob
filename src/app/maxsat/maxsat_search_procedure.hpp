
#pragma once

#include "app/maxsat/maxsat_instance.hpp"
#include "app/maxsat/sat_job_stream.hpp"
#include "app/sat/job/sat_constants.h"
#include "rustsat.h"
#include "util/logger.hpp"
#include "util/sys/terminator.hpp"

#include <climits>
#include <unistd.h>

// C-style clause collector function for RustSAT C API
void maxsat_collect_clause(int lit, void* solver);
// C-style assumption collector function for RustSAT C API
void maxsat_collect_assumption(int lit, void* solver);

// Bundles an iterative solution improving procedure. There can be many at once.
// A MaxSatSearchProcedure is associated 1:1 with a Mallob job stream (SatJobStream).
class MaxSatSearchProcedure {

public:
    // Indicates which solution improving strategy we are following.
    enum SearchStrategy {
        // Try to find a better solution than the currently best one.
        // Many SAT calls followed by a concluding UNSAT call.
        DECREASING,
        // Try to prove that no solution exists for the current lower bound.
        // Many UNSAT calls followed by a concluding SAT call.
        INCREASING,
        // Try to find a solution at the halfway point between the current bounds.
        // Unpredictable sequence of SAT and UNSAT calls. 
        BISECTION,
        // Forbid the last found solution to be found again, in some way that only
        // worse solutions than the best known solution are being prohibited.
        // Many SAT calls followed by a concluding UNSAT call (if you're lucky).
        NAIVE_REFINEMENT
    };

    enum EncodingStrategy {
        NONE,
        GENERALIZED_TOTALIZER,
        DYNAMIC_POLYNOMIAL_WATCHDOG
    };

private:
    static int _running_stream_id;

    const Parameters& _params; // configuration, cmd line arguments
    APIConnector& _api; // for submitting jobs to Mallob
    JobDescription& _desc; // contains our instance to solve and all metadata
    MaxSatInstance& _instance;

    std::string _username;
    SatJobStream _job_stream;

    // vector of 0-separated (hard) clauses to add in the next SAT call
    std::vector<int> _lits_to_add;
    // vector of assumption literals for the next SAT call
    std::vector<int> _assumptions_to_set;
    size_t _current_bound;
    bool _solving {false};
    // holds the assumptions which should be added as permanent units
    // if the result is "satisfiable"
    std::vector<int> _assumptions_to_persist_upon_sat;

    EncodingStrategy _encoding_strat;
    RustSAT::DbGte* _cardi_gte {nullptr};
    RustSAT::DynamicPolyWatchdog* _cardi_dpw {nullptr};

    SearchStrategy _search_strat;
    const std::string _label;

    // only for NAIVE_REFINEMENT strategy
    std::vector<int> _last_found_solution;
    std::vector<MaxSatInstance::ObjectiveTerm> _shuffled_objective;

    int _comb_search_idx {-1};
    int _nb_comb_searchers;

public:
    MaxSatSearchProcedure(const Parameters& params, APIConnector& api, JobDescription& desc,
            MaxSatInstance& instance, SearchStrategy strategy, const std::string& label) :
        _params(params), _api(api), _desc(desc), _instance(instance),
        _username("maxsat#" + std::to_string(_desc.getId())),
        _job_stream(_params, _api, _desc, _running_stream_id++, true),
        _lits_to_add(_instance.formulaData, _instance.formulaData+_instance.formulaSize),
        _current_bound(_instance.upperBound), _search_strat(strategy), _label(label) {

        // Decide on the strategy to use for cardinality encoding.
        _encoding_strat = DYNAMIC_POLYNOMIAL_WATCHDOG; // TODO when to use GTE?
        if (_search_strat == NAIVE_REFINEMENT) _encoding_strat = NONE;
        // Initialize cardinality constraint encoder.
        if (_encoding_strat == DYNAMIC_POLYNOMIAL_WATCHDOG) {
            _cardi_dpw = RustSAT::dpw_new();
            // add each term of the objective function
            for (auto& [factor, lit] : _instance.objective)
                RustSAT::dpw_add(_cardi_dpw, lit, factor);
        }
        if (_encoding_strat == GENERALIZED_TOTALIZER) {
            _cardi_gte = RustSAT::gte_new();
            // add each term of the objective function
            for (auto& [factor, lit] : _instance.objective)
                RustSAT::gte_add(_cardi_gte, lit, factor);
        }
    }
    ~MaxSatSearchProcedure() {
        if (_cardi_gte) RustSAT::gte_drop(_cardi_gte);
        if (_cardi_dpw) RustSAT::dpw_drop(_cardi_dpw);
    }

    void enableCombSearch(int index, int size) {
        _comb_search_idx = index;
        _nb_comb_searchers = size;
    }

    bool isIdle() const {
        return !_solving;
    }

    void enforceNextBound() {

        size_t myLb = _instance.lowerBound;
        size_t myUb = _instance.upperBound;
        if (_comb_search_idx >= 0 && (myUb-myLb) >= _nb_comb_searchers) {
            myLb = _instance.lowerBound + (_comb_search_idx * (_instance.upperBound-_instance.lowerBound)) / _nb_comb_searchers;
            myUb = _instance.lowerBound + ((_comb_search_idx+1) * (_instance.upperBound-_instance.lowerBound)) / _nb_comb_searchers;
            LOG(V3_VERB, "MAXSAT %s Comb search in [%lu,%lu]\n", _label.c_str(), myLb, myUb);
        }

        switch (_search_strat) {
        case INCREASING:
            _current_bound = myLb;
            break;
        case DECREASING:
            _current_bound = _instance.findNextPossibleLowerCost(myUb);
            break;
        case BISECTION:
            _current_bound = myLb + (myUb-myLb) / 2;
            break;
        case NAIVE_REFINEMENT:
            if (_last_found_solution.empty()) {
                // no local solution yet
                if (_instance.bestCost == ULONG_MAX) return; // - solve bound-free
                _last_found_solution = _instance.bestSolution; // - use best solution thus far
            }
            LOG(V4_VVER, "MAXSAT %s Forbidding naive core of last solution ...\n", _label.c_str());
            refineLastSolution();
            break;
        }
        if (_encoding_strat == NONE) return;


        LOG(V4_VVER, "MAXSAT %s Enforcing bound %lu ...\n", _label.c_str(), _current_bound);
        const int prevNbVars = _instance.nbVars;
        // Encode any cardinality constraints that are still missing for the upcoming call.
        if (_encoding_strat == DYNAMIC_POLYNOMIAL_WATCHDOG) {
            RustSAT::dpw_limit_range(_cardi_dpw, _instance.lowerBound, _instance.upperBound,
                &maxsat_collect_clause, this);
            RustSAT::dpw_encode_ub(_cardi_dpw, _current_bound, _current_bound,
                &_instance.nbVars, &maxsat_collect_clause, this);
            RustSAT::dpw_enforce_ub(_cardi_dpw, _current_bound, &maxsat_collect_assumption, this);
            // If the result is SAT, we can add the 1st assumption permanently.
            if (!_assumptions_to_set.empty())
                _assumptions_to_persist_upon_sat.push_back(_assumptions_to_set.front());
        }
        if (_encoding_strat == GENERALIZED_TOTALIZER) {
            RustSAT::gte_encode_ub(_cardi_gte, _current_bound, _current_bound,
                &_instance.nbVars, &maxsat_collect_clause, this);
            RustSAT::gte_enforce_ub(_cardi_gte, _current_bound, &maxsat_collect_assumption, this);
            // If the result is SAT, we can add all assumptions permanently.
            _assumptions_to_persist_upon_sat = _assumptions_to_set;
        }
        LOG(V4_VVER, "MAXSAT %s Enforced bound %lu (%i new vars)\n", _label.c_str(), _current_bound, _instance.nbVars-prevNbVars);
    }

    void solveNonblocking() {
        LOG(V2_INFO, "MAXSAT %s Calling SAT with bound %lu (%i new lits, %i assumptions)\n",
            _label.c_str(), _current_bound, _lits_to_add.size(), _assumptions_to_set.size());
        _job_stream.submitNext(_lits_to_add, _assumptions_to_set);
        _lits_to_add.clear();
        _assumptions_to_set.clear();
        _solving = true;
    }
    bool isNonblockingSolvePending() const {
        return !Terminator::isTerminating() && _job_stream.isPending() && _solving;
    }
    int processNonblockingSolveResult() {
        _solving = false;

        if (Terminator::isTerminating()) return RESULT_UNKNOWN;

        // Job is done - retrieve the result.
        auto result = std::move(_job_stream.getResult());
        const int resultCode = result["result"]["resultcode"];
        if (resultCode == RESULT_UNSAT) {
            // UNSAT
            if (_search_strat == NAIVE_REFINEMENT) {
                // Special case naive refinement: we didn't enforce a certain bound *explicitly*.
                // If we get UNSAT nonetheless, then we were successful in ruling out all solutions
                // matching or exceeding the best cost found so far. Then this is our sharp UNSAT bound.
                _current_bound = _instance.bestCost-1;
            }
            if (_current_bound >= _instance.lowerBound) {
                _instance.lowerBound = _instance.findNextPossibleHigherCost(_current_bound);
                LOG(V2_INFO, "MAXSAT %s Bound %lu unsat - new bounds: (%lu,%lu)\n",
                    _label.c_str(), _current_bound, _instance.lowerBound, _instance.upperBound);
            } else {
                LOG(V2_INFO, "MAXSAT %s Bound %lu unsat - bounds unchanged\n", _label.c_str(), _current_bound);
            }
            return RESULT_UNSAT;
        }
        if (resultCode != RESULT_SAT) {
            // UNKNOWN or something else - presumably because the job was interrupted
            LOG(V2_INFO, "MAXSAT %s Call returned UNKNOWN\n", _label.c_str(), _current_bound);
            return RESULT_UNKNOWN;
        }
        // Formula is SATisfiable.

        // Retrieve the initial model and compute its cost as a first upper bound.
        auto solution = std::move(result["result"]["solution"].get<std::vector<int>>());
        if (_search_strat == NAIVE_REFINEMENT) {
            // remember *any* found solution to forbid it in the next step
            _last_found_solution = solution;
        }
        const size_t cost = _instance.getCostOfModel(solution);
        if (cost < _instance.bestCost) {
            _instance.upperBound = std::min(_instance.upperBound, cost);
            _instance.bestCost = cost;
            _instance.bestSolution = solution;
            LOG(V2_INFO, "MAXSAT %s Bound %lu solved with cost %lu - new bounds: (%lu,%lu)\n",
                _label.c_str(), _current_bound, _instance.bestCost, _instance.lowerBound, _instance.upperBound);
        } else {
            LOG(V2_INFO, "MAXSAT %s Bound %lu solved with cost %lu - bounds unchanged\n",
                _label.c_str(), _current_bound, _instance.bestCost);
        }
        // Since we found SAT, add assumptions as permanent unit clauses where possible.
        for (int asmpt : _assumptions_to_persist_upon_sat) {
            //appendLiteral(asmpt);
            //appendLiteral(0);
        }
        _assumptions_to_persist_upon_sat.clear();

        return RESULT_SAT;
    }

    int solveBlocking() {
        solveNonblocking();
        while (isNonblockingSolvePending()) usleep(1000 * 10); // 10 ms
        return processNonblockingSolveResult();       
    }

    bool isSolvingAttemptObsolete() const {
        assert(_solving);
        // We are solving for (cost <= _current_bound).
        // Case 1: We already know that this cost is impossible to achieve. 
        if (_current_bound < _instance.lowerBound) return true;
        // Case 2: We already know of a better solution than the tested bound.
        // We allow some leniency here since it may be better to keep a job running
        // if its bound is only slightly suboptimal w.r.t. the best known bound. 
        if (_current_bound > 1.01 * _instance.bestCost) return true;
        // Otherwise, the solving attempt is not obsolete.
        return false;
    }

    void interrupt() {
        assert(_solving);
        if (_job_stream.interrupt())
            LOG(V2_INFO, "MAXSAT %s Interrupt solving with bound %i\n", _label.c_str(), _current_bound);
    }

private:

    // We use the best solution and now forbid the solver to select 
    // some (sufficient) subset of the "true" objective literals.
    // Note that this does *not* necessarily result in a monotonic
    // progression of the cost we find.
    void refineLastSolution() {

        size_t addedCost = 0; // count the cost of "true" literals so far
        std::string clauseStr;

        // Code to execute for each term in the objective function.
        auto loopBody = [&](MaxSatInstance::ObjectiveTerm term) {

            // Check if the term was active in the last found solution.
            const int var = std::abs(term.lit);
            assert(var < _last_found_solution.size());
            const int modelLit = _last_found_solution[var];
            assert(modelLit == var || modelLit == -var);

            if (modelLit == term.lit) {
                // This literal was set to true, so it contributed to the cost.
                addedCost += term.factor;
                appendLiteral(-modelLit); // add to combination to forbid
                clauseStr += std::to_string(-modelLit) + " ";
                if (addedCost >= _instance.bestCost) {
                    // The literals we collected up to now sum up to our upper bound (or more),
                    // so forbidding these to go together does not exclude any solution we would still want.
                    // We can thus forbid this set of terms and ignore the remaining ones.
                    return false;
                }
            }
            return true;
        };

        // Traversing the objective in reverse order (i.e., by weight descendingly)
        // should lead to decently short clauses.
        for (int i = _instance.objective.size()-1; i >= 0; i--)
            if (!loopBody(_instance.objective[i])) break;
        appendLiteral(0); // end clause forbidding the last found term combination
        
        LOG(V6_DEBGV, "MAXSAT ADD_CLAUSE %s0\n", clauseStr.c_str());
    }

    // Add a permanent literal to the next SAT call. (0 = end of clause)
    void appendLiteral(int lit) {
        LOG(V6_DEBGV, "MAXSAT %s Append lit %i\n", _label.c_str(), lit);
        _lits_to_add.push_back(lit);
    }
    // Append an assumption for the next SAT call.
    void appendAssumption(int lit) {
        _assumptions_to_set.push_back(lit);
    }

    friend void maxsat_collect_clause(int lit, void *solver);
    friend void maxsat_collect_assumption(int lit, void *solver);
};

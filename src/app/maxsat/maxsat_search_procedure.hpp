
#pragma once

#include "app/maxsat/encoding/cardinality_encoding.hpp"
#include "app/maxsat/encoding/generalized_totalizer.hpp"
#include "app/maxsat/encoding/polynomial_watchdog.hpp"
#include "app/maxsat/encoding/warners_adder.hpp"
#include "app/maxsat/maxsat_instance.hpp"
#include "app/maxsat/sat_job_stream.hpp"
#include "app/sat/job/sat_constants.h"
#include "rustsat.h"
#include "util/logger.hpp"
#include "util/string_utils.hpp"
#include "util/sys/background_worker.hpp"
#include "util/sys/terminator.hpp"
#include "util/params.hpp"
#include "util/sys/thread_pool.hpp"

#include <climits>
#include <memory>
#include <unistd.h>

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
        WARNERS_ADDER,
        DYNAMIC_POLYNOMIAL_WATCHDOG,
        GENERALIZED_TOTALIZER
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
    std::shared_ptr<CardinalityEncoding> _enc;
    bool _shared_encoder;
    volatile bool _is_encoding {false};
    volatile bool _is_done_encoding {false};
    std::future<void> _future_encoder;

    SearchStrategy _search_strat;
    const std::string _label;

    // only for NAIVE_REFINEMENT strategy
    std::vector<int> _last_found_solution;
    std::vector<MaxSatInstance::ObjectiveTerm> _shuffled_objective;

    std::string _desc_label_next_call;

    bool _yield_searcher {false};
    bool _finalized {false};

public:
    MaxSatSearchProcedure(const Parameters& params, APIConnector& api, JobDescription& desc,
            MaxSatInstance& instance, EncodingStrategy encStrat, SearchStrategy searchStrat, const std::string& label) :
        _params(params), _api(api), _desc(desc), _instance(instance),
        _username("maxsat#" + std::to_string(_desc.getId())),
        _job_stream(_params, _api, _desc, _running_stream_id++, true),
        _lits_to_add(_instance.formulaData, _instance.formulaData+_instance.formulaSize),
        _current_bound(_instance.upperBound), _encoding_strat(encStrat), _search_strat(searchStrat), _label(label) {

        _shared_encoder = _params.maxSatSharedEncoder();
        if (!_shared_encoder) {
            if (encStrat == WARNERS_ADDER)
                _enc.reset(new WarnersAdder(_instance.nbVars, _instance.objective));
            if (encStrat == DYNAMIC_POLYNOMIAL_WATCHDOG)
                _enc.reset(new PolynomialWatchdog(_instance.nbVars, _instance.objective));
            if (encStrat == GENERALIZED_TOTALIZER)
                _enc.reset(new GeneralizedTotalizer(_instance.nbVars, _instance.objective));
            _enc->setClauseCollector([&](int lit) {appendLiteral(lit);});
            _enc->setAssumptionCollector([&](int lit) {appendAssumption(lit);});
        }
    }

    void setSharedEncoder(const std::shared_ptr<CardinalityEncoding>& encoder) {
        assert(_shared_encoder);
        _enc = encoder;
    }

    void appendLiterals(const std::vector<int>& litsToAdd) {
        _lits_to_add.insert(_lits_to_add.end(), litsToAdd.begin(), litsToAdd.end());
    }
    void appendAssumptions(const std::vector<int>& assumptions) {
        _assumptions_to_set.insert(_assumptions_to_set.end(), assumptions.begin(), assumptions.end());
    }

    bool isIdle() const {
        return !_solving && !_is_encoding;
    }
    bool isEncoding() const {
        return _is_encoding;
    }

    bool enforceNextBound() {
        if (_yield_searcher) return false;

        const size_t globalLowerBound = _instance.lowerBound;
        const size_t globalUpperBound = _instance.upperBound;
        if (globalUpperBound <= globalLowerBound) return false;

        if (_instance.combSearch) {
            if (!_instance.combSearch->getNextBound(_current_bound)) return false;
        } else {
            switch (_search_strat) {
            case INCREASING:
                _current_bound = globalLowerBound;
                break;
            case DECREASING:
                _current_bound = _instance.findNextPossibleLowerCost(globalUpperBound);
                break;
            case BISECTION:
                _current_bound = globalLowerBound + (globalUpperBound-globalLowerBound) / 2;
                break;
            case NAIVE_REFINEMENT:
                if (_last_found_solution.empty()) {
                    // no local solution yet
                    if (_instance.bestCost == ULONG_MAX) return true; // - solve bound-free
                    _last_found_solution = _instance.bestSolution; // - use best solution thus far
                }
                LOG(V4_VVER, "MAXSAT %s Forbidding naive core of last solution ...\n", _label.c_str());
                refineLastSolution();
                break;
            }
        }
        if (_encoding_strat == NONE) return true;

        LOG(V4_VVER, "MAXSAT %s Enforcing bound %lu ...\n", _label.c_str(), _current_bound);
        // Encode any cardinality constraints that are still missing for the upcoming call.
        
        // If we use a shared encoder, the constraints are already encoded, but we need to re-link 
        // the collectors for the enforceBound() call. Otherwise, we encode the constraints now.
        if (_shared_encoder) {
            _enc->setClauseCollector([&](int lit) {appendLiteral(lit);});
            _enc->setAssumptionCollector([&](int lit) {appendAssumption(lit);});
        }

        assert(!_future_encoder.valid());
        _is_encoding = true;
        _future_encoder = ProcessWideThreadPool::get().addTask([&, min=globalLowerBound, ub=_current_bound, max=globalUpperBound]() {
            if (!_shared_encoder) {
                _enc->encode(min, ub, max);
            }
            // Enforce our current bound (assumptions only)
            _enc->enforceBound(ub);
            // If the result is SAT, we can add the 1st assumption permanently.
            //if (!_assumptions_to_set.empty())
            //    _assumptions_to_persist_upon_sat.push_back(_assumptions_to_set.front());
            _is_done_encoding = true;
        });
        return true;
    }
    bool isDoneEncoding() {
        if (!_is_encoding) return false;
        if (!_is_done_encoding) return false;
        _future_encoder.get();
        _is_done_encoding = false;
        _is_encoding = false;
        return true;
    }

    void solveNonblocking() {
        assert(!_is_encoding && !_future_encoder.valid());
        LOG(V2_INFO, "MAXSAT %s Calling SAT with bound %lu (%i new lits, %i assumptions)\n",
            _label.c_str(), _current_bound, _lits_to_add.size(), _assumptions_to_set.size());
        LOG(V2_INFO, "MAXSAT Literals: %s\n", StringUtils::getSummary(_lits_to_add).c_str());
        LOG(V2_INFO, "MAXSAT Assumptions: %s\n", StringUtils::getSummary(_assumptions_to_set).c_str());
        _job_stream.submitNext(_lits_to_add, _assumptions_to_set,
            _desc_label_next_call,
            // We let the position of the tested bound influence the job's priority
            // as a tie-breaker for the scheduler - considering the highest bounds
            // as the most useful to give resources to.
            1.0f + 0.01f * (_current_bound - _instance.lowerBound) / (float) (_instance.upperBound - _instance.lowerBound));
        _lits_to_add.clear();
        _assumptions_to_set.clear();
        _desc_label_next_call = "";
        _solving = true;
    }
    bool isNonblockingSolvePending() const {
        return _job_stream.isPending() && _solving;
    }
    int processNonblockingSolveResult() {
        _solving = false;

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
                if (_instance.combSearch)
                    _instance.combSearch->stopTestingAndUpdateLower(_current_bound);
            } else {
                LOG(V2_INFO, "MAXSAT %s Bound %lu unsat - bounds unchanged\n", _label.c_str(), _current_bound);
                if (_instance.combSearch)
                    _instance.combSearch->stopTestingWithoutUpdates(_current_bound);
            }
            return RESULT_UNSAT;
        }
        if (resultCode != RESULT_SAT) {
            // UNKNOWN or something else - presumably because the job was interrupted
            LOG(V2_INFO, "MAXSAT %s Call returned UNKNOWN\n", _label.c_str(), _current_bound);
            if (_instance.combSearch)
                _instance.combSearch->stopTestingWithoutUpdates(_current_bound);
            return RESULT_UNKNOWN;
        }
        // Formula is SATisfiable.

        // Retrieve the initial model and compute its cost as a first upper bound.
        auto solution = result["result"]["solution"].get<std::vector<int>>();
        if (_search_strat == NAIVE_REFINEMENT) {
            // remember *any* found solution to forbid it in the next step
            _last_found_solution = solution;
        }
        const size_t cost = _instance.getCostOfModel(solution);
        assert(cost <= _current_bound || log_return_false("[ERROR] MAXSAT Returned solution for bound %lu has cost %lu!\n", _current_bound, cost));
        if (cost < _instance.bestCost) {
            _instance.upperBound = std::min(_instance.upperBound, cost);
            _instance.bestCost = cost;
            _instance.bestSolution = solution;
            LOG(V2_INFO, "MAXSAT %s Bound %lu solved with cost %lu - new bounds: (%lu,%lu)\n",
                _label.c_str(), _current_bound, _instance.bestCost, _instance.lowerBound, _instance.upperBound);
            if (_instance.combSearch)
                _instance.combSearch->stopTestingAndUpdateUpper(_current_bound, cost-1);
        } else {
            LOG(V2_INFO, "MAXSAT %s Bound %lu solved with cost %lu - bounds unchanged\n",
                _label.c_str(), _current_bound, _instance.bestCost);
            if (_instance.combSearch)
                _instance.combSearch->stopTestingWithoutUpdates(_current_bound);
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
        while (isNonblockingSolvePending()) usleep(1000); // 1 ms
        return processNonblockingSolveResult();       
    }

    bool isSolvingAttemptObsolete() const {
        assert(_solving);
        // With the "naive refinement" strategy, we don't encode bounds explicitly,
        // so this search does not become obsolete with improved bounds per se.
        if (_search_strat == NAIVE_REFINEMENT) return false;
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

    void interrupt(bool terminate = false) {
        assert(_solving);
        if (terminate) _yield_searcher = true;
        if (_job_stream.interrupt())
            LOG(V2_INFO, "MAXSAT %s Interrupt solving with bound %i\n", _label.c_str(), _current_bound);
    }

    void setDescriptionLabelForNextCall(const std::string& label) {
        _desc_label_next_call = label;
    }

    void setGroupId(const std::string& groupId, int minVar = -1, int maxVar = -1) {
        _job_stream.setGroupId(groupId, minVar, maxVar);
    }

    size_t getCurrentBound() const {
        return _current_bound;
    }

    bool canBeFinalized() {
        return !isEncoding() || isDoneEncoding();
    }
    void finalize() {
        if (_finalized) return;
        _job_stream.finalize();
        _finalized = true;
    }

    ~MaxSatSearchProcedure() {
        while (!canBeFinalized()) {usleep(1000);}
        finalize();
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
};

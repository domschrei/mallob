
#pragma once

#include "app/incsat/inc_sat_controller.hpp"
#include "app/maxsat/encoding/cardinality_encoding.hpp"
#include "app/maxsat/encoding/generalized_totalizer.hpp"
#include "app/maxsat/encoding/polynomial_watchdog.hpp"
#include "app/maxsat/encoding/warners_adder.hpp"
#include "app/sat/stream/internal_sat_job_stream_processor.hpp"
#include "app/sat/stream/mallob_sat_job_stream_processor.hpp"
#include "app/maxsat/maxsat_instance.hpp"
#include "app/sat/stream/sat_job_stream.hpp"
#include "app/maxsat/solution_writer.hpp"
#include "app/sat/data/theories/integer_term.hpp"
#include "app/sat/data/theories/theory_specification.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/sat/stream/sat_job_stream_garbage_collector.hpp"
#include "app/sat/stream/sat_job_stream_processor.hpp"
#include "app/sat/stream/wrapped_sat_job_stream.hpp"
#include "core/dtask_tracker.hpp"
#include "interface/api/api_connector.hpp"
#include "rustsat.h"
#include "scheduling/core_allocator.hpp"
#include "util/logger.hpp"
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
        GENERALIZED_TOTALIZER,
        VIRTUAL
    };

private:

    const Parameters& _params; // configuration, cmd line arguments
    APIConnector& _api; // for submitting jobs to Mallob
    JobDescription& _desc; // contains our instance to solve and all metadata
    MaxSatInstance& _instance;
    int _nb_orig_vars;

    std::unique_ptr<IncSatController> _stream_wrapper;

    // vector of 0-separated (hard) clauses to add in the next SAT call
    std::vector<int> _lits_to_add;
    // vector of assumption literals for the next SAT call
    std::vector<int> _assumptions_to_set;
    Checksum _chksum; // for debugging
    size_t _current_bound {ULONG_MAX};
    bool _solving {false};
    // holds the assumptions which should be added as permanent units
    // if the result is "satisfiable"
    std::vector<int> _assumptions_to_persist_upon_sat;

    EncodingStrategy _encoding_strat;
    std::shared_ptr<CardinalityEncoding> _enc;
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
    bool _initialized {false};

    std::shared_ptr<SolutionWriter> _sol_writer;

public:
    MaxSatSearchProcedure(const Parameters& params, APIConnector& api, JobDescription& desc, DTaskTracker& tracker,
            MaxSatInstance& instance, EncodingStrategy encStrat, SearchStrategy searchStrat, const std::string& label) :
        _params(params), _api(api), _desc(desc), _instance(instance),
        _lits_to_add(_instance.formulaData, _instance.formulaData+_instance.formulaSize),
        _current_bound(ULONG_MAX), _encoding_strat(encStrat), _search_strat(searchStrat), _label(label) {

        _stream_wrapper.reset(new IncSatController(_params, _api, _desc, tracker));

        _nb_orig_vars = _instance.nbVars; // before cardinality constraint encodings!

        if (encStrat == WARNERS_ADDER)
            _enc.reset(new WarnersAdder(_instance.nbVars, _instance.objective));
        if (encStrat == DYNAMIC_POLYNOMIAL_WATCHDOG)
            _enc.reset(new PolynomialWatchdog(_instance.nbVars, _instance.objective));
        if (encStrat == GENERALIZED_TOTALIZER)
            _enc.reset(new GeneralizedTotalizer(_instance.nbVars, _instance.objective));
        if (_enc) {
            _enc->setClauseCollector([&](int lit) {appendLiteral(lit);});
            _enc->setAssumptionCollector([&](int lit) {appendAssumption(lit);});
        }

        if (_encoding_strat == VIRTUAL) {
            LOG(V2_INFO, "MAXSAT Setting up virtual \"theory\" encoding ...\n");
            IntegerTerm termSum(IntegerTerm::ADD);
            for (auto [coeffWeight, coeffLit] : _instance.objective) {
                IntegerTerm termWeight(IntegerTerm::CONSTANT);
                termWeight.inner() = coeffWeight;
                IntegerTerm termLit(IntegerTerm::LITERAL);
                termLit.inner() = coeffLit;
                IntegerTerm termCoeff(IntegerTerm::MULTIPLY);
                termCoeff.addChildAndTryFlatten(std::move(termWeight));
                termCoeff.addChildAndTryFlatten(std::move(termLit));
                termSum.addChildAndTryFlatten(std::move(termCoeff));
            }
            IntegerRule rule(IntegerRule::MINIMIZE, std::move(termSum));
            TheorySpecification spec({std::move(rule)});
            std::string specStr = spec.toStr();
            specStr.erase(std::remove_if(specStr.begin(), specStr.end(), ::isspace), specStr.end());
            _stream_wrapper->getMallobProcessor()->setInnerObjective(specStr);
        }
    }

    void setSolutionWriter(std::shared_ptr<SolutionWriter> solutionWriter) {
        _sol_writer = solutionWriter;
    }

    void appendLiterals(const std::vector<int>& litsToAdd) {
        _lits_to_add.insert(_lits_to_add.end(), litsToAdd.begin(), litsToAdd.end());
    }

    bool isIdle() const {
        return !_solving && !_is_encoding;
    }
    bool isEncoding() const {
        return _is_encoding;
    }

    bool enforceNextBound(size_t boundOverride = -1UL) {

        const size_t globalLowerBound = _instance.lowerBound;
        const size_t globalUpperBound = _instance.upperBound;

        if (boundOverride == -1UL) {
            if (!findNextBound()) return false;
        } else {
            _current_bound = boundOverride;
        }
        if (_encoding_strat == NONE || _search_strat == NAIVE_REFINEMENT || _current_bound == ULONG_MAX)
            return true;

        LOG(V4_VVER, "MAXSAT %s Enforcing bound %lu ...\n", _label.c_str(), _current_bound);
        // Encode any cardinality constraints that are still missing for the upcoming call.

        assert(!_future_encoder.valid());
        _is_encoding = true;
        if (_enc) {
            _future_encoder = ProcessWideThreadPool::get().addTask([&, min=globalLowerBound, ub=_current_bound, max=globalUpperBound]() {
                CoreAllocator::Allocation ca(1);
                _enc->encode(min, ub, max);
                // Enforce our current bound (assumptions only)
                _enc->enforceBound(ub);
                // If the result is SAT, we can add the 1st assumption permanently.
                //if (!_assumptions_to_set.empty())
                //    _assumptions_to_persist_upon_sat.push_back(_assumptions_to_set.front());
                _is_done_encoding = true;
            });
        } else {
            _is_done_encoding = true;
        }
        return true;
    }
    bool isDoneEncoding() {
        if (!_is_encoding) return false;
        if (!_is_done_encoding) return false;
        if (_future_encoder.valid()) _future_encoder.get();
        _is_done_encoding = false;
        _is_encoding = false;
        return true;
    }

    void solveNonblocking() {
        assert(!_is_encoding && !_future_encoder.valid());

        if (_params.useChecksums()) for (int l : _lits_to_add) _chksum.combine(l);
        auto hash = _chksum;
        if (_params.useChecksums()) for (int a : _assumptions_to_set) hash.combine(a);
        LOG(V2_INFO, "MAXSAT %s Calling SAT %s (%i new lits, %i assumptions, chk %lu,%x)\n",
            _label.c_str(), _current_bound==ULONG_MAX ? "bound-free" : ("with bound " + std::to_string(_current_bound)).c_str(),
            _lits_to_add.size(), _assumptions_to_set.size(), hash.count(), hash.get());
        if (_params.verbosity() >= V4_VVER) {
            LOG(V4_VVER, "MAXSAT Literals: %s\n", StringUtils::getSummary(_lits_to_add).c_str());
            LOG(V4_VVER, "MAXSAT Assumptions: %s\n", StringUtils::getSummary(_assumptions_to_set).c_str());
        }

        if (!_initialized && _stream_wrapper->hasStream()) {
            _stream_wrapper->getMallobProcessor()->setInitialSize(
                _instance.nbVars,
                _desc.getAppConfiguration().fixedSizeEntryToInt("__NC"));
            _initialized = true;
        }

        _stream_wrapper->solveNextRevisionNonblocking(std::move(_lits_to_add),
            std::move(_assumptions_to_set), _desc_label_next_call);

        _lits_to_add.clear();
        _assumptions_to_set.clear();
        _desc_label_next_call = "";
        _solving = true;
    }
    bool isNonblockingSolvePending() {
        return _stream_wrapper->getStream().isNonblockingSolvePending() && _solving;
    }
    int processNonblockingSolveResult() {
        _solving = false;

        // Job is done - retrieve the result.
        auto [resultCode, solution] = _stream_wrapper->getStream().getNonblockingSolveResult();
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
                if (_instance.intervalSearch)
                    _instance.intervalSearch->stopTestingAndUpdateLower(_current_bound);
            } else {
                LOG(V2_INFO, "MAXSAT %s Bound %lu unsat - bounds unchanged\n", _label.c_str(), _current_bound);
                if (_instance.intervalSearch)
                    _instance.intervalSearch->stopTestingWithoutUpdates(_current_bound);
            }
            _current_bound = ULONG_MAX;
            return RESULT_UNSAT;
        }
        if (resultCode != RESULT_SAT) {
            // UNKNOWN or something else - presumably because the job was interrupted
            LOG(V2_INFO, "MAXSAT %s Call returned UNKNOWN\n", _label.c_str(), _current_bound);
            if (_instance.intervalSearch)
                _instance.intervalSearch->stopTestingWithoutUpdates(_current_bound);
            _current_bound = ULONG_MAX;
            return RESULT_UNKNOWN;
        }
        // Formula is SATisfiable.

        // Retrieve the initial model and compute its cost as a first upper bound.
        if (_search_strat == NAIVE_REFINEMENT) {
            // remember *any* found solution to forbid it in the next step
            _last_found_solution = solution;
        }
        const size_t cost = _instance.getCostOfModel(solution);
        if (cost > _current_bound) {
            /*
            std::string reportFilename = _params.logDirectory() + "/erroneous-maxsat-model." + result["name"].get<std::string>();
            {
                std::ofstream ofs(reportFilename);
                ofs << "MaxSAT searcher " << _label << std::endl;
                ofs << "Internal job ID: #" << result["internal_id"].get<int>() << std::endl;
                ofs << "Job literals: perhaps present at " << _params.logDirectory() + "/satjobstream.joblits." + result["name"].get<std::string>() << std::endl;
                ofs << "Job assumptions: perhaps present at " << _params.logDirectory() + "/satjobstream.jobassumptions." + result["name"].get<std::string>() << std::endl;
                ofs << "Found model: perhaps present at " << _params.solutionToFile() + "." + std::to_string(result["internal_id"].get<int>())
                    + "." + std::to_string(result["internal_revision"].get<int>()) << std::endl;
                ofs << "Enforced cost: " << _current_bound << " or lower" << std::endl;
                ofs << "Cost obtained from model: " << cost << std::endl;
                for (auto& term : _instance.objective) {
                    const int termLit = term.lit;
                    assert(std::abs(termLit) < solution.size());
                    const int modelLit = solution[std::abs(termLit)];
                    assert(termLit == modelLit || termLit == -modelLit);
                    if (modelLit == termLit) {
                        ofs << "  model literal " << modelLit << " : cost " << term.factor << " incurred" << std::endl;
                    } else {
                        ofs << "  model literal " << modelLit << " : no cost (" << term.factor << ") incurred" << std::endl;
                    }
                }
            }
            LOG(V0_CRIT, "[ERROR] MAXSAT Model for bound %lu has cost %lu - report written to %s\n",
                _current_bound, cost, reportFilename.c_str());
            */
            LOG(V0_CRIT, "[ERROR] MAXSAT Model for bound %lu has cost %lu!\n", _current_bound, cost);
            abort();
        }
        if (cost < _instance.bestCost) {
            _instance.upperBound = std::min(_instance.upperBound, cost);
            _instance.bestCost = cost;
            _instance.bestSolution = solution;
            _instance.bestSolution.resize(_nb_orig_vars+1); // truncate to original variables
            _instance.bestSolutionPreprocessLayer = _instance.preprocessLayer;
            LOG(V2_INFO, "MAXSAT %s Bound %lu solved with cost %lu - new bounds: (%lu,%lu)\n",
                _label.c_str(), _current_bound, _instance.bestCost, _instance.lowerBound, _instance.upperBound);
            if (_sol_writer) _sol_writer->appendSolution(cost, solution);
            if (_instance.intervalSearch) {
                _instance.intervalSearch->stopTestingAndUpdateUpper(_current_bound, cost);
            }
        } else {
            LOG(V2_INFO, "MAXSAT %s Bound %lu solved with cost %lu - bounds unchanged\n",
                _label.c_str(), _current_bound, _instance.bestCost);
            if (_instance.intervalSearch)
                _instance.intervalSearch->stopTestingWithoutUpdates(_current_bound);
        }
        _assumptions_to_persist_upon_sat.clear();
        _current_bound = ULONG_MAX;
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
        // if its bound is only slightly suboptimal w.r.t. the best known cost.
        if (_current_bound > 1.01 * _instance.bestCost) return true;
        // Otherwise, the solving attempt is not obsolete.
        return false;
    }

    void interrupt(bool terminate = false) {
        assert(_solving);
        if (terminate) _yield_searcher = true;
        if (_stream_wrapper->getStream().interrupt()) {
            if (_current_bound == ULONG_MAX)
                LOG(V2_INFO, "MAXSAT %s Interrupt bound-free solving\n", _label.c_str());
            else
                LOG(V2_INFO, "MAXSAT %s Interrupt solving with bound %lu\n", _label.c_str(), _current_bound);
        }
    }

    void setDescriptionLabelForNextCall(const std::string& label) {
        _desc_label_next_call = label;
    }

    void setGroupId(const std::string& groupId, int minVar = -1, int maxVar = -1) {
        _stream_wrapper->getMallobProcessor()->setGroupId(groupId, minVar, maxVar);
    }

    size_t getCurrentBound() const {
        return _current_bound;
    }

    bool canBeFinalized() {
        return !isEncoding() || isDoneEncoding();
    }
    void finalize() {
        if (_finalized) return;
        _finalized = true;
        _stream_wrapper->finalize();
    }

    ~MaxSatSearchProcedure() {
        while (!canBeFinalized()) {usleep(1000);}
        finalize();
    }

private:

    bool findNextBound() {
        if (_yield_searcher) return false;

        const size_t globalLowerBound = _instance.lowerBound;
        const size_t globalUpperBound = _instance.upperBound;
        if (globalUpperBound <= globalLowerBound) return false;

        if (_instance.intervalSearch) {
            if (!_instance.intervalSearch->getNextBound(_current_bound)) return false;
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
        return true;
    }

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

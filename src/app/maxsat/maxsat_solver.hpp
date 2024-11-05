
#pragma once

#include "app/maxsat/encoding/cardinality_encoding.hpp"
#include "app/maxsat/encoding/generalized_totalizer.hpp"
#include "app/maxsat/encoding/polynomial_watchdog.hpp"
#include "app/maxsat/encoding/warners_adder.hpp"
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
#include <memory>
#include <unistd.h>
#include "robin_set.h"
#include "util/logger.hpp"
// external
#include "rustsat.h"
#include "util/string_utils.hpp"
#include "util/sys/terminator.hpp"
#include "util/params.hpp"

void maxsat_collect_clause_for_shared_encoding(int lit, void* data);

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

    bool _shared_encoder;
    MaxSatSearchProcedure::EncodingStrategy _encoding_strat;
    std::vector<int> _shared_lits_to_add;

    float _start_time;

public:
    // Initializes the solver instance and parses the description's formula.
    MaxSatSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc) {

        LOG(V2_INFO, "Mallob client-side MaxSAT solver, by Jeremias Berg & Dominik Schreiber\n");
        parseFormula();

        _shared_encoder = _params.maxSatSharedEncoder();
        switch (_params.maxSatCardinalityEncoding()) {
        case 0: {_encoding_strat = MaxSatSearchProcedure::WARNERS_ADDER; break;}
        case 1: {_encoding_strat = MaxSatSearchProcedure::DYNAMIC_POLYNOMIAL_WATCHDOG; break;}
        case 2: {_encoding_strat = MaxSatSearchProcedure::GENERALIZED_TOTALIZER; break;}
        case 3: {_encoding_strat = pickCardinalityEncoding(); break;}
        case 4: {_encoding_strat = MaxSatSearchProcedure::VIRTUAL; break;}
        default: {_encoding_strat = MaxSatSearchProcedure::NONE; break;}
        }
        LOG(V2_INFO, "MAXSAT Using cardinality encoding %i\n", _encoding_strat);
    }

    // Perform exact MaxSAT solving and return an according result.
    JobResult solve() {

        _start_time = Timer::elapsedSeconds();

        // Template for the result we will return in the end
        JobResult r;
        r.id = _desc.getId();
        r.revision = 0;
        r.result = RESULT_UNKNOWN;

        // Just for debugging
        _instance->print();

        // TODO If the preprocessor found a non-trivial upper bound,
        // can we extract a solution from it?
        if (_instance->lowerBound == _instance->upperBound) {
            r.result = RESULT_OPTIMUM_FOUND;
            r.setSolution({}); // TODO
            LOG(V2_INFO, "MAXSAT OPTIMAL COST %lu\n", _instance->upperBound);
            Logger::getMainInstance().flush();
            return r;
        }

        // Parse the user-provided sequence of search strategies.
        std::string searchStrats = std::string(_params.maxSatNumSearchers(), 'd');
        const int nbWorkers = _params.numWorkers() == -1 ? MyMpi::size(MPI_COMM_WORLD) : _params.numWorkers();
        // Loop over each specified search strategy
        for (int i = 0; i < searchStrats.size(); i++) {
            char c = searchStrats[i];
            // Check if we have enough workers in the system for another job stream
            if (nbWorkers <= _searches.size()) {
                LOG(V1_WARN, "MAXSAT [WARN] Truncating number of parallel search strategies to %i due to lack of workers\n",
                    nbWorkers);
                break;
            }
            // Initialize search procedure
            _searches.emplace_back(initializeSearchProcedure(c, i, searchStrats.size()));
            _searches.back()->setDescriptionLabelForNextCall("base-formula");

            // If everybody uses their own encoder, we can still put all of them in the same cross-sharing group
            // due to the consistent naming of variables across all encoders.
            if (!_shared_encoder) {
                _searches.back()->setGroupId("consistent-logic"/*, 1, _instance->nbVars*/);
            }
        }
        assert(!_searches.empty());

        // If the best known upper bound is trivial, we first perform a solving attempt
        // without encoding any constraints. If we know a non-trivial bound from preprocessing,
        // we skip this and instead begin encoding it right away.
        const bool solveWithoutBounds = _instance->upperBound == _instance->sumOfWeights;
        if (solveWithoutBounds) {
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

            if (_encoding_strat == MaxSatSearchProcedure::VIRTUAL) {
                // if SAT is returned in this mode, the solution was proven optimal
                r.result = RESULT_OPTIMUM_FOUND;
                r.setSolution(std::move(_instance->bestSolution));
                LOG(V2_INFO, "MAXSAT OPTIMAL COST %lu\n", _instance->upperBound);
                Logger::getMainInstance().flush();
                return r;
            }
        }

        // Run the initial formula revision through ALL searches, so that everyone has the same one.
        if (_searches.size() > 1) for (auto& search : _searches) {
            if (solveWithoutBounds && search == _searches.front())
                continue; // this search has already been run once
            search->solveNonblocking();
            search->interrupt();
        }

        // Initialize interval search if needed
        if (_instance->intervalSearch)
            _instance->intervalSearch->init(_instance->lowerBound, _instance->upperBound);

        if (_shared_encoder) {
            // Initialize cardinality constraint encoder.
            std::shared_ptr<CardinalityEncoding> encoder;
            if (_encoding_strat == MaxSatSearchProcedure::WARNERS_ADDER)
                encoder.reset(new WarnersAdder(_instance->nbVars, _instance->objective));
            if (_encoding_strat == MaxSatSearchProcedure::DYNAMIC_POLYNOMIAL_WATCHDOG)
                encoder.reset(new PolynomialWatchdog(_instance->nbVars, _instance->objective));
            if (_encoding_strat == MaxSatSearchProcedure::GENERALIZED_TOTALIZER)
                encoder.reset(new GeneralizedTotalizer(_instance->nbVars, _instance->objective));
            encoder->setClauseCollector([&](int lit) {_shared_lits_to_add.push_back(lit);});
            encoder->setAssumptionCollector([&](int lit) {abort();}); // no assumptions at this stage!
            encoder->encode(_instance->lowerBound, _instance->upperBound, _instance->upperBound);

            // Add the encoder and its encoding to each search
            for (auto& search : _searches) {
                search->setSharedEncoder(encoder);
                search->setDescriptionLabelForNextCall("initial-bounds");
                search->setGroupId("common-logic"); // enable cross job clause sharing
                search->appendLiterals(_shared_lits_to_add);
            }
        }

        // Main loop for solution improving search.
        std::list<std::unique_ptr<MaxSatSearchProcedure>> searchesToFinalize;
        float timeOfLastChange = Timer::elapsedSeconds();
        while (!isTimeoutHit() && _instance->lowerBound < _instance->upperBound && !_searches.empty()) {
            // Loop over all search strategies
            bool change = false;
            for (auto it = _searches.begin(); it != _searches.end(); ++it) {
                auto& search = *it;
                // In a solve call right now?
                if (!search->isIdle() && !search->isEncoding()) {
                    if (!search->isNonblockingSolvePending()) {
                        // Current solving procedure has finished:
                        // apply the result to the MaxSAT instance
                        (void) search->processNonblockingSolveResult();
                        change = true;
                        if (_instance->lowerBound >= _instance->upperBound)
                            break;
                    } else if (search->isSolvingAttemptObsolete()) {
                        // The current bounds have made this solve call obsolete:
                        // interrupt it.
                        search->interrupt();
                        change = true;
                    }
                }
                // No solving procedure ongoing nor pending?
                if (search->isIdle()) {
                    // Compute and enforce the next bound for this strategy
                    change = true;
                    bool goOn = search->enforceNextBound();
                    if (!goOn) {
                        // This search procedure does not want to continue: stop and remove it.
                        searchesToFinalize.emplace_back();
                        std::swap(search, searchesToFinalize.back());
                        it = _searches.erase(it);
                        --it;
                        continue;
                    }
                }
                if (search->isDoneEncoding()) {
                    // Launch a SAT job
                    search->solveNonblocking();
                    change = true;
                }
            }
            if (_instance->lowerBound >= _instance->upperBound)
                break;
            // Wait a bit if nothing changed
            if (!change) {
                usleep(1000); // 1 ms
                if (_params.maxSatFocusPeriod() > 0 && Timer::elapsedSeconds() - timeOfLastChange > _params.maxSatFocusPeriod()
                        && _searches.size() > _params.maxSatFocusMin()) {
                    // cancel the searcher at the lowest bound
                    MaxSatSearchProcedure* lowest {nullptr};
                    for (auto& search : _searches) {
                        if (search->isNonblockingSolvePending() &&
                            (!lowest || search->getCurrentBound() < lowest->getCurrentBound())) {
                            lowest = search.get();
                        }
                    }
                    if (lowest) {
                        LOG(V2_INFO, "MAXSAT focus: cancel search at bound %lu\n", lowest->getCurrentBound());
                        lowest->interrupt(true);
                        change = true;
                    }
                }
            }
            if (change) timeOfLastChange = Timer::elapsedSeconds();

            // delete old searches where possibe
            for (auto it = searchesToFinalize.begin(); it != searchesToFinalize.end(); ++it) {
                auto& search = *it;
                if (search->canBeFinalized()) {
                    search->finalize();
                    it = searchesToFinalize.erase(it);
                    --it;
                }
            }
        }

        // Did we actually find an optimal result?
        if (_instance->lowerBound >= _instance->upperBound) {
            // construct & return final job result
            r.result = RESULT_OPTIMUM_FOUND;
            r.setSolution(std::move(_instance->bestSolution));
            LOG(V2_INFO, "MAXSAT OPTIMAL COST %lu\n", _instance->upperBound);
            Logger::getMainInstance().flush();
        }

        LOG(V4_VVER, "MAXSAT trying to stop all searches ...\n");

        // Make sure to stop all searches
        while (!isTimeoutHit()) {
            bool allIdle = true;
            for (auto& search : _searches) {
                if (!search->isIdle() && !search->isEncoding()) {
                    if (!search->isNonblockingSolvePending()) search->processNonblockingSolveResult();
                    else search->interrupt();
                }
                if (search->isDoneEncoding()) {}
                if (!search->isIdle()) allIdle = false;
            }
            if (allIdle) break;
            usleep(1000 * 1); // 1 ms
        }
        // Now clean up all searches
        _searches.clear();
        searchesToFinalize.clear();

        return r;
    }

private:
    // Parses the formula contained in _desc and initializes _instance accordingly.
    void parseFormula() {

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
        assert(nbObjectiveTerms > 0);
        pos--;
        assert(fPtr[pos] == 0);
        // pos now points at the separation zero right before the objective
        // hard clauses end at the separation zero to the objective
        _instance.reset(new MaxSatInstance(fPtr, pos));
        _instance->lowerBound = lb;
        _instance->upperBound = ub;

        // Now actually parse the objective function
        ++pos;
        tsl::robin_set<size_t> uniqueFactors;
        while (_instance->objective.size() < nbObjectiveTerms) {
            size_t factor = * (size_t*) (fPtr+pos);
            // MaxPRE already flips the literals' polarity
            int lit = (_params.maxPre() ? 1 : -1) * fPtr[pos+2];
            assert(factor != 0);
            assert(lit != 0);
            _instance->objective.push_back({factor, lit});
            uniqueFactors.insert(factor);
            pos += 3;
        }
        _instance->nbUniqueWeights = uniqueFactors.size();
        // Sort the objective terms by weight in increasing order
        // (may help to find the required steps to take in solution-improving search)
        std::sort(_instance->objective.begin(), _instance->objective.end(),
            [&](const MaxSatInstance::ObjectiveTerm& termLeft, const MaxSatInstance::ObjectiveTerm& termRight) {
            return termLeft.factor < termRight.factor;
        });

        _instance->nbVars = _desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
        _instance->sumOfWeights = 0;
        for (auto term : _instance->objective) _instance->sumOfWeights += term.factor;
        _instance->upperBound = std::min(_instance->upperBound, _instance->sumOfWeights);
        _instance->bestCost = ULONG_MAX;

        _instance->intervalSearch.reset(new IntervalSearch(_params.maxSatIntervalSkew()));
    }

    // Heuristic picking a suitable cardinality encoding based on the objective function's properties.
    // Obtained by a mix of educated guesses and 1-minute runs on MaxSAT Eval'23 instances.
    MaxSatSearchProcedure::EncodingStrategy pickCardinalityEncoding() {

        // Large objective function or very large sum of weights: Fallback to Adder.
        if (_instance->objective.size() > 10'000 || _instance->sumOfWeights > 1'000'000'000'000UL)
            return MaxSatSearchProcedure::WARNERS_ADDER;

        // Very small sum of weights and few unique weights: GTE can be used.
        if (_instance->sumOfWeights <= 100 && _instance->nbUniqueWeights <= 20)
            return MaxSatSearchProcedure::GENERALIZED_TOTALIZER;

        // Otherwise, default case of DPW.
        return MaxSatSearchProcedure::DYNAMIC_POLYNOMIAL_WATCHDOG;
    }

    MaxSatSearchProcedure* initializeSearchProcedure(char c, int index, int nbTotal) {
        // Parse search strategy
        MaxSatSearchProcedure::SearchStrategy searchStrat;
        std::string label = std::to_string(index) + ":";
        switch (c) {
        case 'd':
            searchStrat = MaxSatSearchProcedure::DECREASING;
            label += "DEC";
            break;
        case 'i':
            searchStrat = MaxSatSearchProcedure::INCREASING;
            label += "INC";
            break;
        case 'b':
            searchStrat = MaxSatSearchProcedure::BISECTION;
            label += "BIS";
            break;
        case 'r':
            searchStrat = MaxSatSearchProcedure::NAIVE_REFINEMENT;
            label += "NRE";
            break;
        }
        // Initialize search procedure
        auto p = new MaxSatSearchProcedure(_params, _api, _desc,
            *_instance, _encoding_strat, searchStrat, label);
        return p;
    }

    bool isTimeoutHit() const {
        if (_params.timeLimit() > 0 && Timer::elapsedSeconds() >= _params.timeLimit())
            return true;
        if (_desc.getWallclockLimit() > 0 && (Timer::elapsedSeconds() - _start_time) >= _desc.getWallclockLimit())
            return true;
        if (Terminator::isTerminating())
            return true;
        return false;
    }
};

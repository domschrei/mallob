
#pragma once

#include "app/maxsat/encoding/cardinality_encoding.hpp"
#include "app/maxsat/encoding/generalized_totalizer.hpp"
#include "app/maxsat/encoding/polynomial_watchdog.hpp"
#include "app/maxsat/encoding/warners_adder.hpp"
#include "app/maxsat/maxsat_instance.hpp"
#include "app/maxsat/maxsat_search_procedure.hpp"
#include "app/maxsat/parse/maxsat_reader.hpp"
#include "app/maxsat/solution_writer.hpp"
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
#include "scheduling/core_allocator.hpp"
#include "util/logger.hpp"
#include "rustsat.h" // external
#include "util/string_utils.hpp"
#include "util/sys/terminator.hpp"
#include "util/params.hpp"
#include "util/sys/watchdog.hpp"
#if MALLOB_USE_MAXPRE == 1
#include "parse/static_maxsat_parser_store.hpp"
#endif

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
    int _cores_allocated;

    std::unique_ptr<MaxSatInstance> _instance; // the problem instance we're solving

    bool _shared_encoder;
    MaxSatSearchProcedure::EncodingStrategy _encoding_strat;
    std::vector<int> _shared_lits_to_add;

    float _start_time = -1;

    struct InstanceUpdate {
        std::vector<int> formula;
        std::vector<std::pair<uint64_t, int>> objective;
        int nbVars {0};
        int nbReadClauses {0};
        int preprocessLayer {0};
        unsigned long lowerBound;
        unsigned long upperBound;
        void prepareForNext(unsigned long lowerBound, unsigned long upperBound) {
            formula.clear();
            objective.clear();
            nbVars = 0;
            nbReadClauses = 0;
            this->lowerBound = lowerBound;
            this->upperBound = upperBound;
            preprocessLayer++;
        }
    } _instance_update;
    struct UpdateResult {
        bool boundsImproved {false};
        bool instanceImproved {false};
        bool error {false};
    } _update_result;
    bool _maxpre_run_done {false};
    std::future<void> _fut_instance_update;
    std::atomic_long _maxpre_tid;

public:
    // Initializes the solver instance and parses the description's formula.
    MaxSatSolver(const Parameters& params, APIConnector& api, JobDescription& desc) :
        _params(params), _api(api), _desc(desc) {

        LOG(V2_INFO, "Mallob client-side MaxSAT solver, by Jeremias Berg & Dominik Schreiber\n");
        _cores_allocated = ProcessWideCoreAllocator::get().requestCores(1);
        parseFormula();
        pickEncodingStrategy();
    }
    MaxSatSolver(MaxSatSolver&& other) = delete;
    ~MaxSatSolver() {
        ProcessWideCoreAllocator::get().returnCores(_cores_allocated);
#if MALLOB_USE_MAXPRE == 1
        // join with background MaxPRE preprocessor 
        if (_fut_instance_update.valid()) {
            while (_maxpre_tid == 0) {usleep(1000);}
            Watchdog watchdog(_params.watchdog(), 1000, true, [tid = _maxpre_tid.load()]() {Process::writeTrace(tid);});
            watchdog.setAbortPeriod(15'000);
            int jobId = _desc.getId();
            LOG(V2_INFO, "MAXSAT interrupt MaxPRE of #%i asynchronously\n", jobId);
            StaticMaxSatParserStore::get(jobId)->interruptAsynchronously();
            _fut_instance_update.get();
            LOG(V2_INFO, "MAXSAT MaxPRE stopped\n");
        }
        // delete MaxPRE preprocessor
        StaticMaxSatParserStore::erase(_desc.getId());
#endif
    }

    // Perform exact MaxSAT solving and return an according result.
    JobResult solve(int updateLayer = 0) {

        // holds all active streams of Mallob jobs and allows interacting with them
        std::list<std::unique_ptr<MaxSatSearchProcedure>> searches;
        std::shared_ptr<SolutionWriter> writer;
        if (_params.maxSatSolutionFile.isSet())
            writer.reset(new SolutionWriter(_instance->nbVars, _params.maxSatSolutionFile(), _params.compressModels()));

        if (_start_time < 0) _start_time = Timer::elapsedSeconds();

        // Template for the result we will return in the end
        JobResult r;
        r.id = _desc.getId();
        r.revision = 0;
        r.result = RESULT_UNKNOWN;

        // Just for debugging
        _instance->print(updateLayer);

        _maxpre_run_done = false;
#if MALLOB_USE_MAXPRE == 1
        auto parser = StaticMaxSatParserStore::get(_desc.getId());
        // Conditions for running a concurrent preprocessing:
        // * within max. number of preprocessing iterations
        // * non-zero timeout specified for "post" MaxPRE runs
        // * if we try the same techniques as in the initial parsing,
        //   the last preprocessing must have been interrupted
        if (updateLayer < 10 && _params.maxPreTimeoutPost() > 0
            && (_params.maxPreTechniques() != _params.maxPreTechniquesPost()
                || parser->lastCallInterrupted())) {
            launchImprovingMaxPreRun(updateLayer);
        }
#endif

        // Parse the user-provided sequence of search strategies.
        size_t nbSearchers = std::min((size_t)_params.maxSatNumSearchers(), (_instance->upperBound - _instance->lowerBound) + 1);
        std::string searchStrats = std::string(nbSearchers, 'd');
        const int nbWorkers = _params.numWorkers() == -1 ? MyMpi::size(MPI_COMM_WORLD) : _params.numWorkers();
        // Loop over each specified search strategy
        for (int i = 0; i < searchStrats.size(); i++) {
            char c = searchStrats[i];
            // Check if we have enough workers in the system for another job stream
            if (nbWorkers <= searches.size()) {
                LOG(V1_WARN, "MAXSAT [WARN] Truncating number of parallel search strategies to %i due to lack of workers\n",
                    nbWorkers);
                break;
            }
            // Initialize search procedure
            searches.emplace_back(initializeSearchProcedure(c, i, searchStrats.size()));
            searches.back()->setDescriptionLabelForNextCall("base-formula-" + std::to_string(updateLayer));

            // If everybody uses their own encoder, we can still put all of them in the same cross-sharing group
            // due to the consistent naming of variables across all encoders.
            if (!_shared_encoder) {
                searches.back()->setGroupId("consistent-logic-" + std::to_string(updateLayer)/*, 1, _instance->nbVars*/);
            }

            if (writer) searches.back()->setSolutionWriter(writer);
        }
        assert(!searches.empty());

        // In some cases, it makes sense to first perform a solving attempt without constraints.
        const bool noSolutionPresent = _instance->bestCost == ULONG_MAX;
        const bool firstSolveWithoutBounds = _encoding_strat == MaxSatSearchProcedure::VIRTUAL
            || _instance->objective.empty() || noSolutionPresent;
        if (firstSolveWithoutBounds) {
            // Initial SAT call: just solve the hard clauses.
            // We just use the first specified search strategy for this task.
            MaxSatSearchProcedure* search = searches.front().get();
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
        }

        if (_instance->objective.empty() || _encoding_strat == MaxSatSearchProcedure::VIRTUAL
                || _instance->lowerBound == _instance->bestCost) {
            // the solution is already proven optimal
            r.result = RESULT_OPTIMUM_FOUND;
            r.setSolution(reconstructSolutionToOriginalProblem(false));
            LOG(V2_INFO, "MAXSAT OPTIMAL COST %lu\n", _instance->bestCost);
            Logger::getMainInstance().flush();
            return r;
        } else if (firstSolveWithoutBounds) {
            // Recount the solution cost to get the most accurate possible bound
            (void) reconstructSolutionToOriginalProblem(true);
        }

        // Run the initial formula revision through ALL searches, so that everyone has the same one.
        if (searches.size() > 1) for (auto& search : searches) {
            if (firstSolveWithoutBounds && search == searches.front())
                continue; // this search has already been run once
            search->solveNonblocking();
            search->interrupt();
        }

        // Initialize interval search if needed
        if (_instance->intervalSearch) {
            // As the "max cost to test", use either the best known upper bound or,
            // if we have a "constructive" upper bound, the best known cost minus one
            _instance->intervalSearch->init(_instance->lowerBound,
                std::min(_instance->upperBound, _instance->bestCost-1));
        }

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
            for (auto& search : searches) {
                search->setSharedEncoder(encoder);
                search->setDescriptionLabelForNextCall("initial-bounds-" + std::to_string(updateLayer));
                search->setGroupId("common-logic-" + std::to_string(updateLayer)); // enable cross job clause sharing
                search->appendLiterals(_shared_lits_to_add);
            }
        }

        // Main loop for solution improving search.
        std::list<std::unique_ptr<MaxSatSearchProcedure>> searchesToFinalize;
        bool changeSinceLastFocus = true;
        float timeOfLastChange = Timer::elapsedSeconds();
        while (!isTimeoutHit() && _instance->lowerBound < _instance->bestCost && !searches.empty()) {
            // Loop over all search strategies
            bool change = false;
            bool stagnation = false;
            for (auto it = searches.begin(); it != searches.end(); ++it) {
                auto& search = *it;
                // In a solve call right now?
                if (!search->isIdle() && !search->isEncoding()) {
                    if (!search->isNonblockingSolvePending()) {
                        // Current solving procedure has finished:
                        // apply the result to the MaxSAT instance
                        (void) search->processNonblockingSolveResult();
                        change = true;
                        if (_instance->lowerBound >= _instance->bestCost)
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
                        // But make sure not to delete the only remaining search this way.
                        if (searches.size() == 1) {
                            stagnation = true;
                            break;
                        }
                        searchesToFinalize.emplace_back();
                        std::swap(search, searchesToFinalize.back());
                        it = searches.erase(it);
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
            if (stagnation || _instance->lowerBound >= _instance->bestCost)
                break;

            // Wait a bit if nothing changed
            if (change) {
                changeSinceLastFocus = true;
                timeOfLastChange = Timer::elapsedSeconds();
            } else {
                usleep(1000); // 1 ms
                if (_params.maxSatFocusPeriod() > 0 && Timer::elapsedSeconds() - timeOfLastChange > _params.maxSatFocusPeriod()
                        && searches.size() > _params.maxSatFocusMin() && changeSinceLastFocus) {
                    // cancel the searcher at the lowest bound
                    MaxSatSearchProcedure* lowest {nullptr};
                    for (auto& search : searches) {
                        if (search->isNonblockingSolvePending() &&
                            (!lowest || search->getCurrentBound() < lowest->getCurrentBound())) {
                            lowest = search.get();
                        }
                    }
                    if (lowest) {
                        LOG(V2_INFO, "MAXSAT focus: cancel search at bound %lu\n", lowest->getCurrentBound());
                        lowest->interrupt(true);
                        // make sure that some time elapses AND some change was observed (interrupt!)
                        // before allowing for the next focusing
                        changeSinceLastFocus = false;
                        timeOfLastChange = Timer::elapsedSeconds();
                    }
                }
            }

            // delete old searches where possible
            tryDeleteOldSearches(searchesToFinalize);

#if MALLOB_USE_MAXPRE == 1
            // concurrent improving preprocessing run done?
            if (_maxpre_run_done) {
                _maxpre_run_done = false;
                _fut_instance_update.get();
                _maxpre_tid = 0;
                LOG(V2_INFO, "MAXSAT processing MaxPRE result\n");

                // Did the preprocessor find improved bounds?
                if (_update_result.boundsImproved) {
                    // update bounds with preprocessing results
                    const bool lowerImproved = _instance_update.lowerBound > _instance->lowerBound;
                    _instance->lowerBound = std::max(_instance_update.lowerBound, _instance->lowerBound);
                    const bool upperImproved = _instance_update.upperBound < _instance->upperBound;
                    _instance->upperBound = std::min(_instance_update.upperBound, _instance->upperBound);
                    LOG(V2_INFO, "MAXSAT improved bounds found by MaxPRE - new bounds: (%lu,%lu)\n",
                        _instance->lowerBound, _instance->upperBound);
                    if (lowerImproved) _instance->intervalSearch->stopTestingAndUpdateLower(_instance->lowerBound);
                    if (upperImproved) {
                        // The new upper bound is not "constructive", so we should add 1
                        // to get the hypothetical "best known cost" for interval search
                        _instance->intervalSearch->stopTestingAndUpdateUpper(ULONG_MAX, _instance->upperBound+1);
                    }
                }

                // Did the preprocessor improve the instance itself to a notable degree?
                if (_update_result.instanceImproved) {
                    // cleanup (has to happen before update)
                    LOG(V2_INFO, "MAXSAT improvement found by MaxPRE: restart searches\n");
                    tryStopAllSearches(searches);
                    if (!isTimeoutHit()) {
                        for (auto& search : searches) searchesToFinalize.push_back(std::move(search));
                        searches.clear();
                        while (!searchesToFinalize.empty()) tryDeleteOldSearches(searchesToFinalize);
                        // update
                        updateInstance(_instance_update); // nukes and rewrites instance
                        // try again on updated instance
                        // NOTE: may call launchImprovingMaxSatRun internally
                        return solve(updateLayer+1);
                    }
                } else if (!_update_result.error && updateLayer < 10 && StaticMaxSatParserStore::get(_desc.getId())->lastCallInterrupted()) {
                    // not run until completion yet: retry concurrent preprocessing with higher limit
                    updateLayer++;
                    launchImprovingMaxPreRun(updateLayer);
                }
            }
#endif
        }

        LOG(V4_VVER, "MAXSAT trying to stop all searches ...\n");
        tryStopAllSearches(searches);

        // Did we find *some* solution?
        if (_instance->bestCost < ULONG_MAX) {
            r.result = RESULT_SAT;
        }
        // Were we able to find tight bounds?
        if (_instance->lowerBound >= _instance->upperBound) {
            // -- yes
            assert(_instance->lowerBound == _instance->upperBound);
            // Do we have an *optimal* solution?
            if (_instance->bestCost == _instance->upperBound) {
                // -- yes
                r.result = RESULT_OPTIMUM_FOUND;
            } else if (!isTimeoutHit()) {
                // -- no: tight bounds are known, but we do not have a corresponding solution yet.
                // Make one more SAT call to find such a solution.
                LOG(V2_INFO, "MAXSAT final SAT call to find solution of optimal cost %lu ...\n", _instance->upperBound);
                assert(!searches.empty());
                auto& search = searches.front();
                bool ok = search->enforceNextBound(_instance->upperBound);
                assert(ok);
                while (!search->isDoneEncoding()) usleep(1000);
                int resultCode = search->solveBlocking(); // could still be cancelled
                if (resultCode == SAT) {
                    assert(_instance->bestCost == _instance->upperBound);
                    r.result = RESULT_OPTIMUM_FOUND;
                }
                LOG(V4_VVER, "MAXSAT once again trying to stop all searches ...\n");
                tryStopAllSearches(searches);
            }
        }
        // Report the found solution.
        if (_instance->bestCost < ULONG_MAX) {
            r.setSolution(reconstructSolutionToOriginalProblem(r.result != RESULT_OPTIMUM_FOUND));
            LOG(V2_INFO, "MAXSAT %s COST %lu\n", r.result == RESULT_OPTIMUM_FOUND ? "OPTIMAL" : "BEST FOUND", _instance->bestCost);
            if (r.result == RESULT_OPTIMUM_FOUND && writer) writer->concludeOptimal();
            Logger::getMainInstance().flush();
        }

        // Clean up all searches with their (incremental) Mallob jobs
        LOG(V2_INFO, "MAXSAT finalizing all searches ...\n");
        while (!searchesToFinalize.empty()) tryDeleteOldSearches(searchesToFinalize);

        // Now all searches can be cleaned up by leaving this method
        LOG(V2_INFO, "MAXSAT exiting\n");
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
        Logger::getMainInstance().flush();
        assert(nbObjectiveTerms >= 0);
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
#if MALLOB_USE_MAXPRE == 1
            int lit = (_params.maxPre() ? 1 : -1) * fPtr[pos+2];
#else
            int lit = -1 * fPtr[pos+2];
#endif
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

    void pickEncodingStrategy() {
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

#if MALLOB_USE_MAXPRE == 1
    void launchImprovingMaxPreRun(int updateLayer) {
        assert(!_maxpre_run_done);

        _instance_update.prepareForNext(_instance->lowerBound, _instance->upperBound);
        LOG(V3_VERB, "MAXSAT calling MaxPRE concurrently\n");
        _fut_instance_update = ProcessWideThreadPool::get().addTask([this, updateLayer]() {
            _maxpre_tid = Proc::getTid();
            auto& update = _instance_update;
            auto parser = StaticMaxSatParserStore::get(_desc.getId());

            // Decide on time limit based on the current preprocessing layer
            // and time remaining until external (job-specific and global) time limits are hit
            float time = Timer::elapsedSeconds();
            float timeLimit = _params.maxPreTimeoutPost() * std::pow(2, updateLayer);
            if (_desc.getWallclockLimit() > 0)
                timeLimit = std::min(timeLimit, std::max(0.01f, _desc.getWallclockLimit() - (time - _start_time)));
            if (_params.timeLimit() > 0)
                timeLimit = std::min(timeLimit, std::max(0.01f, _params.timeLimit() - time));

            // Perform actual preprocessing
            parser->preprocess(_params.maxPreTechniquesPost(), 0, timeLimit);

            // Report finished MaxPRE run
            const float timePreprocess = Timer::elapsedSeconds() - time;
            parser->getInstance(update.formula, update.objective, update.nbVars, update.nbReadClauses);
            LOG(V3_VERB, "MAXSAT MaxPRE layer=%i stat lits:%i vars:%i cls:%i obj:%lu lb:%lu ub:%lu timegranted:%.4f timetaken:%.4f\n",
                update.preprocessLayer, update.formula.size(), update.nbVars, update.nbReadClauses,
                update.objective.size(), parser->get_lb(), parser->get_ub(),
                timeLimit, timePreprocess);

            // Assess result
            if (update.formula.size() == 0 && update.nbVars == 0) {
                // Error in MaxPRE
                _update_result.error = true;
                _maxpre_run_done = true;
                return;
            }
            _update_result.boundsImproved = (parser->get_lb() > update.lowerBound || parser->get_ub() < update.upperBound);
            update.lowerBound = parser->get_lb();
            update.upperBound = parser->get_ub();
            _update_result.instanceImproved = update.nbVars <= 0.9 * _instance->nbVars
                || update.formula.size() <= 0.9 * _instance->formulaSize
                || update.objective.size() <= 0.9 * _instance->objective.size();
            _maxpre_run_done = true;
        });
    }

    void updateInstance(InstanceUpdate& update) {

        // remember important fields from any prior attempts
        auto lb = _instance->lowerBound;
        auto ub = _instance->upperBound;
        auto bestCost = _instance->bestCost;
        auto bestSolution = std::move(_instance->bestSolution);
        int preprocessorLayerOfBestSolution = _instance->bestSolutionPreprocessLayer;

        // construct new instance object
        _instance.reset(new MaxSatInstance(update.formula.data(), update.formula.size()));
        _instance->nbVars = update.nbVars;
        _desc.getAppConfiguration().updateFixedSizeEntry("__NV", update.nbVars);
        _desc.getAppConfiguration().updateFixedSizeEntry("__NC", update.nbReadClauses);
        _desc.getAppConfiguration().updateFixedSizeEntry("__NO", (int)update.objective.size());
        _instance->lowerBound = std::max(update.lowerBound, lb);
        _instance->upperBound = std::min(update.upperBound, ub);
        _instance->preprocessLayer = update.preprocessLayer;
        _instance->bestCost = bestCost;
        _instance->bestSolution = std::move(bestSolution);
        _instance->bestSolutionPreprocessLayer = preprocessorLayerOfBestSolution;
        tsl::robin_set<size_t> uniqueFactors;
        for (auto [weight, lit] : update.objective) {
            _instance->objective.push_back({weight, lit});
            uniqueFactors.insert(weight);
        }
        _instance->nbUniqueWeights = uniqueFactors.size();
        // Sort the objective terms by weight in increasing order
        // (may help to find the required steps to take in solution-improving search)
        std::sort(_instance->objective.begin(), _instance->objective.end(),
            [&](const MaxSatInstance::ObjectiveTerm& termLeft, const MaxSatInstance::ObjectiveTerm& termRight) {
            return termLeft.factor < termRight.factor;
        });
        _instance->sumOfWeights = 0;
        for (auto term : _instance->objective) _instance->sumOfWeights += term.factor;
        _instance->upperBound = std::min(_instance->upperBound, _instance->sumOfWeights);
        _instance->intervalSearch.reset(new IntervalSearch(_params.maxSatIntervalSkew()));
        pickEncodingStrategy();
    }
#endif

    // Heuristic picking a suitable cardinality encoding based on the objective function's properties.
    // Obtained by a mix of educated guesses and 1-minute runs on MaxSAT Eval'23 instances.
    MaxSatSearchProcedure::EncodingStrategy pickCardinalityEncoding() {

        // For really tiny objective functions, GTE should always be the cheapest and most direct option.
        if (_instance->objective.size() <= 5) return MaxSatSearchProcedure::GENERALIZED_TOTALIZER;

        // Large objective function or very large sum of weights
        // or very large base formula with decently large objective: Fallback to Adder.
        if (_instance->objective.size() > 10'000 || _instance->sumOfWeights > 1'000'000'000'000UL
                || (_instance->formulaSize > 10'000'000 && _instance->objective.size() > 5'000))
            return MaxSatSearchProcedure::WARNERS_ADDER;

        // Very small sum of weights, few unique weights, and a not too large problem
        // in terms of literals or objective terms: GTE can be used.
        if (_instance->sumOfWeights <= 100 && _instance->nbUniqueWeights <= 20
                && (_instance->formulaSize <= 10'000'000 || _instance->objective.size() <= 25))
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

    void tryStopAllSearches(std::list<std::unique_ptr<MaxSatSearchProcedure>>& searches) {
        while (true) {
            bool allIdle = true;
            for (auto& search : searches) {
                if (!search->isIdle() && !search->isEncoding()) {
                    if (!search->isNonblockingSolvePending()) search->processNonblockingSolveResult();
                    else search->interrupt();
                }
                if (search->isDoneEncoding()) {}
                if (!search->isIdle()) allIdle = false;
            }
            if (allIdle) return;
            usleep(1000 * 1); // 1 ms
        }
    }

    void tryDeleteOldSearches(std::list<std::unique_ptr<MaxSatSearchProcedure>>& searchesToFinalize) {
        for (auto it = searchesToFinalize.begin(); it != searchesToFinalize.end(); ++it) {
            auto& search = *it;
            if (search->canBeFinalized()) {
                search->finalize();
                it = searchesToFinalize.erase(it);
                --it;
            }
        }
    }

    std::vector<int> reconstructSolutionToOriginalProblem(bool recountCost) {
#if MALLOB_USE_MAXPRE == 1
        auto parser = StaticMaxSatParserStore::get(_desc.getId());
        size_t cost = _instance->bestCost;
        std::vector<int> sol = parser->reconstruct(_instance->bestSolution,
            recountCost ? &cost : nullptr,
            _instance->bestSolutionPreprocessLayer, true, 1);
        if (recountCost) {
            LOG(V2_INFO, "MAXSAT MAXPRE cost recount: %lu -> %lu\n", _instance->bestCost, cost);
            _instance->bestCost = cost;
            _instance->upperBound = std::min(_instance->upperBound, cost);
        }
#else
        std::vector<int> sol = _instance->bestSolution;
        size_t cost = _instance->bestCost;
#endif
        sol[0] = sol.size();
        sol.resize(sol.size() + 2);
        * (unsigned long*) (sol.data()+sol.size()-2) = cost;
        return sol;
    }
};

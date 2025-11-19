
#pragma once

#include "app/sat/data/portfolio_sequence.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/sat/parse/serialized_formula_parser.hpp"
#include "app/sat/solvers/kissat.hpp"
#include "app/sat/solvers/lingeling.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "data/job_description.hpp"
#include "scheduling/core_allocator.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/thread_pool.hpp"
#include <atomic>
#include <future>

class SatPreprocessor {

private:
    const Parameters& _params;
    const JobDescription& _desc;
    bool _run_lingeling {false};
    CoreAllocator::Allocation _core_alloc;

    std::unique_ptr<Lingeling> _lingeling;
    std::unique_ptr<Kissat> _kissat;
    std::future<void> _fut_lingeling;
    std::future<void> _fut_kissat;
    std::atomic_int _solver_result {0};
    std::atomic_int _nb_running {0};
    std::vector<int> _solution;

public:
    SatPreprocessor(const Parameters& params, JobDescription& desc, bool runLingeling) :
        _params(params), _desc(desc), _run_lingeling(runLingeling), _core_alloc(1 + _run_lingeling) {}
    ~SatPreprocessor() {
        join(false);
        if (_kissat) _kissat->cleanUp();
        if (_lingeling) _lingeling->cleanUp();
    }

    void init() {
        SolverSetup setup;
        setup.logger = &Logger::getMainInstance();
        setup.numVars = _desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
        setup.numOriginalClauses = _desc.getAppConfiguration().fixedSizeEntryToInt("__NC");
        setup.solverType = 'p';
        setup.preprocessSequentialSweepComplete = _params.preprocessSequentialSweepComplete.val;
        // setup.shared_sweeping = _params.sharedSweeping.val;
        float t0 = Timer::elapsedSeconds();
        _kissat.reset(new Kissat(setup));
        float t1 = Timer::elapsedSeconds();

        LOG(V3_VERB, "SATWP Starting sequential preprocessor\n");
        LOG(V2_INFO, "SATWP STARTUP (PREPRO) Kissat init duration: %f ms \n", (t1-t0)*1000);
        _nb_running++;
        _fut_kissat = ProcessWideThreadPool::get().addTask([&]() {
            loadFormulaToSolver(_kissat.get());
            LOG(V2_INFO, "SATWP PREPRO running kissat\n");
            t0 = Timer::elapsedSeconds();
            int res = _kissat->solve(0, nullptr);
            t1 = Timer::elapsedSeconds();
            LOG(V2_INFO, "SATWP PREPRO kissat done, result %i\n", res);
            LOG(V2_INFO, "SATWP SEQPREPRO_TIME %f sec\n", t1-t0);
            if (res != RESULT_UNKNOWN) {
                int expected = 0;
                if (_solver_result.compare_exchange_strong(expected, res)) {
                    if (_solver_result == RESULT_SAT) _solution = _kissat->getSolution();
                }
            }
            _nb_running--;
        });
        if (_run_lingeling) {
            setup.solverType = 'l';
            setup.flavour = PortfolioSequence::PREPROCESS;
            _lingeling.reset(new Lingeling(setup));
            _nb_running++;
            _fut_lingeling = ProcessWideThreadPool::get().addTask([&]() {
                loadFormulaToSolver(_lingeling.get());
                LOG(V2_INFO, "SATWP PREPRO running Lingeling\n");
                int res = _lingeling->solve(0, nullptr);
                LOG(V2_INFO, "SATWP PREPRO Lingeling done, result %i\n", res);
                if (res != RESULT_UNKNOWN) {
                    int expected = 0;
                    if (_solver_result.compare_exchange_strong(expected, res)) {
                        if (_solver_result == RESULT_SAT) _solution = _lingeling->getSolution();
                    }
                }
                _nb_running--;
            });
        }
    }

    bool done() {
        // Return allocated cores as needed
        int nbRunning = _nb_running.load(std::memory_order_relaxed);
        if (nbRunning >= 0 && nbRunning < _core_alloc.getNbAllocated())
            _core_alloc.returnCores(_core_alloc.getNbAllocated() - nbRunning);
        // Did we already find a result? Is everyone done?
        bool done = _solver_result.load(std::memory_order_relaxed) != 0 || nbRunning == 0;
        if (nbRunning == 0) _nb_running.store(-1, std::memory_order_relaxed);
        return done;
    }
    int getResultCode() const {
        return _solver_result;
    }
    std::vector<int>&& getSolution() {
        return std::move(_solution);
    }

    bool hasPreprocessedFormula() {
        return _kissat->hasPreprocessedFormula();
    }
    std::vector<int>&& extractPreprocessedFormula() {
        return _kissat->extractPreprocessedFormula();
    }
    // void resetPreprocessedFormula() {
        // _kissat->resetPreprocessedFormula();
    // }

    // Interrupt any preprocessing, no more need for a result
    void interrupt() {
        _kissat->interrupt();
        if (_lingeling) _lingeling->interrupt();
    }
    void join(bool onlyWaitForModel) {
        if (!onlyWaitForModel && _fut_lingeling.valid()) _fut_lingeling.get(); // wait for solver thread to return
        if (_fut_kissat.valid()) _fut_kissat.get(); // wait for solver thread to return
    }

    void reconstructSolution(std::vector<int>& solution) {
        _kissat->reconstructSolutionFromPreprocessing(solution);
    }

private:
    void loadFormulaToSolver(PortfolioSolverInterface* slv) {
        SerializedFormulaParser parser(Logger::getMainInstance(), _desc.getFormulaPayload(0), _desc.getFormulaPayloadSize(0));
        if (_params.compressFormula()) parser.setCompressed();
        int lit;
        while (parser.getNextLiteral(lit)) {
            slv->addLiteral(lit);
        }
        slv->diversify(0);
    }
};

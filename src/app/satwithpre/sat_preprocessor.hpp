
#pragma once

#include "app/sat/data/portfolio_sequence.hpp"
#include "app/sat/job/sat_constants.h"
#include "app/sat/solvers/kissat.hpp"
#include "app/sat/solvers/lingeling.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "data/job_description.hpp"
#include "util/logger.hpp"
#include "util/sys/thread_pool.hpp"
#include <atomic>
#include <future>

class SatPreprocessor {

private:
    const JobDescription& _desc;
    bool _run_lingeling {false};

    std::unique_ptr<Lingeling> _lingeling;
    std::unique_ptr<Kissat> _kissat;
    std::future<void> _fut_lingeling;
    std::future<void> _fut_kissat;
    std::atomic_int _solver_result {0};
    std::atomic_int _nb_running {0};
    std::vector<int> _solution;

public:
    SatPreprocessor(JobDescription& desc, bool runLingeling) : _desc(desc), _run_lingeling(runLingeling) {}
    ~SatPreprocessor() {
        join(false);
        if (_kissat) _kissat->cleanUp();
        if (_lingeling) _lingeling->cleanUp();
    }

    void init() {
        SolverSetup setup;
        setup.logger = &Logger::getMainInstance();
        setup.numVars = _desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
        setup.solverType = 'p';
        _kissat.reset(new Kissat(setup));
        _nb_running++;
        _fut_kissat = ProcessWideThreadPool::get().addTask([&]() {
            loadFormulaToSolver(_kissat.get());
            LOG(V2_INFO, "PREPRO running Kissat\n");
            int res = _kissat->solve(0, nullptr);
            LOG(V2_INFO, "PREPRO Kissat done, result %i\n", res);
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
                LOG(V2_INFO, "PREPRO running Lingeling\n");
                int res = _lingeling->solve(0, nullptr);
                LOG(V2_INFO, "PREPRO Lingeling done, result %i\n", res);
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
        // Did we already find a result?
        if (_solver_result.load(std::memory_order_relaxed) != 0)
            return true;
        bool done = _nb_running.load(std::memory_order_relaxed) == 0;
        if (done) _nb_running.store(-1, std::memory_order_relaxed);
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
        const int* lits = _desc.getFormulaPayload(0);
        for (int i = 0; i < _desc.getFormulaPayloadSize(0); i++) {
            slv->addLiteral(lits[i]);
        }
        slv->diversify(0);
    }
};


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
#include "util/sys/process.hpp"
#include "util/sys/thread_pool.hpp"
#include <atomic>
#include <fstream>
#include <future>

#if MALLOB_USE_SATSUMA == 2
#include "app/satwithpre/ext_satsuma_caller.hpp"
#elif MALLOB_USE_SATSUMA == 1
#include "ISatsumaPreprocessor.h"
#include "ICnf2wl.h"
#endif

class SatPreprocessor {

private:
    int numberOfVariables;
    const Parameters& _params;
    JobDescription& _desc;
    bool _run_lingeling {false};
    bool _run_satsuma {false};
    bool _chain_kissat_after_satsuma {false};

#if MALLOB_USE_SATSUMA == 2
    bool _satsuma_external = true;
#else
    bool _satsuma_external = false;
#endif

    volatile bool _kissat_initialized {false};
    volatile bool _kissat_interrupted {false};
    CoreAllocator::Allocation _core_alloc;

    std::unique_ptr<Lingeling> _lingeling;
    std::unique_ptr<Kissat> _kissat;
    std::future<void> _fut_lingeling;
    std::future<void> _fut_kissat;
    std::future<void> _fut_satsuma;
    std::atomic_int _solver_result {0};
    std::atomic_int _nb_running {0};
    std::vector<int> _solution;

#if MALLOB_USE_SATSUMA == 1
    std::unique_ptr<satsuma::ISatsumaPreprocessor> _satsuma_preprocessor;
#endif
#if MALLOB_USE_SATSUMA == 2
    std::unique_ptr<ExtSatsumaCaller> _ext_satsuma_caller;
#endif

public:
    SatPreprocessor(const Parameters& params, JobDescription& desc, bool runLingeling) :
        _params(params), _desc(desc), _run_lingeling(runLingeling), _core_alloc(1 + _run_lingeling) {
#if MALLOB_USE_SATSUMA
        _run_satsuma = _params.preprocessSatsuma();
        _chain_kissat_after_satsuma = _params.chainKissatAfterSatsuma();
#endif
    }
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
        if (!_run_satsuma){
            setup.solverType = 'p';
            _kissat.reset(new Kissat(setup));
            _nb_running++;
            _kissat_initialized = true;
            _fut_kissat = ProcessWideThreadPool::get().addTask([&]() {
                if (!_kissat_interrupted) loadFormulaToSolver(_kissat.get());
                LOG(V2_INFO, "PREPRO running Kissat\n");
                int res = _kissat_interrupted ? 0 : _kissat->solve(0, nullptr);
                LOG(V2_INFO, "PREPRO Kissat done, result %i\n", res);
                if (res != RESULT_UNKNOWN) {
                    int expected = 0;
                    if (_solver_result.compare_exchange_strong(expected, res)) {
                        if (_solver_result == RESULT_SAT) _solution = _kissat->getSolution();
                    }
                }
                _nb_running--;
            });
        }

        if (_run_satsuma) {
            _nb_running++;
            _fut_satsuma = ProcessWideThreadPool::get().addTask([&]() {
            	LOG(V2_INFO, "PREPRO running Satsuma\n");

#if MALLOB_USE_SATSUMA == 2
                _ext_satsuma_caller.reset(new ExtSatsumaCaller(_params, _desc, "ExtSatsuma"));
                auto satsumaRes = _ext_satsuma_caller->callBlocking();
                if (satsumaRes == ExtSatsumaCaller::UNSAT) {
                    int expected = 0;
                    _solver_result.compare_exchange_strong(expected, 20);
                    _nb_running--;
                    return;
                }
                if (satsumaRes != ExtSatsumaCaller::SUCCESS) abort();
#else
                _satsuma_preprocessor = satsuma::create_preprocessor();
                std::unique_ptr<satsuma::ICnf2wl> formula = satsuma::create_cnf2wl();
                loadFormulaToCnf2wl(*formula);
                _satsuma_preprocessor->set_save_as_Formula(true);
                // hier vielleicht echten logger mit bestimmter verbosity
                std::ofstream dev_null("/dev/null");
                _satsuma_preprocessor->set_log_output(&dev_null);
                _satsuma_preprocessor->preprocess(*formula);
#endif
                LOG(V2_INFO, "PREPRO Satsuma done\n");

                if (_chain_kissat_after_satsuma && !_kissat_interrupted) {
#if MALLOB_USE_SATSUMA == 2
                    std::vector<int> satsumaResult = std::move(_ext_satsuma_caller->getPreprocessedFormula());
#else
                    std::vector<int> satsumaResult = std::move(_satsuma_preprocessor->extractPreprocessedFormula());
#endif
                    SolverSetup kissatSetup;
                    kissatSetup.logger = &Logger::getMainInstance();
                    assert(satsumaResult.size() > 2);
                    kissatSetup.numOriginalClauses = satsumaResult.back(); satsumaResult.pop_back();
                    kissatSetup.numVars = satsumaResult.back(); satsumaResult.pop_back();
                    kissatSetup.solverType = 'p';
                    _kissat.reset(new Kissat(kissatSetup));
                    _kissat_initialized = true;
                    if (!_kissat_interrupted) loadFormulaFromExtracted(_kissat.get(), satsumaResult);
                    LOG(V2_INFO, "PREPRO running Kissat\n");
                    int res = _kissat_interrupted ? 0 : _kissat->solve(0, nullptr);
                    LOG(V2_INFO, "PREPRO Kissat done, result %i\n", res);
                    if (res != RESULT_UNKNOWN) {
                        int expected = 0;
                        if (_solver_result.compare_exchange_strong(expected, res)) {
                            if (_solver_result == RESULT_SAT) _solution = _kissat->getSolution();
                        }
                    }
                }
                
                _nb_running--;
            });
        }

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
        if (!_run_satsuma || _chain_kissat_after_satsuma) {
            if (_kissat_initialized) {
                return _kissat->hasPreprocessedFormula();
            }
            return false;
        }
#if MALLOB_USE_SATSUMA == 2
        return _ext_satsuma_caller && _ext_satsuma_caller->hasPreprocessedFormula();
#elif MALLOB_USE_SATSUMA == 1
        return _satsuma_preprocessor->hasPreprocessedFormula();
#else
        return false;
#endif
    }

    std::vector<int> extractPreprocessedFormula() {
        if (!_run_satsuma || (_chain_kissat_after_satsuma && _kissat_initialized)) {
            return std::move(_kissat->extractPreprocessedFormula());
        }
        LOG(V2_INFO, "PREPRO extracting Satsuma\n");
#if MALLOB_USE_SATSUMA == 2
        return std::move(_ext_satsuma_caller->getPreprocessedFormula());
#elif MALLOB_USE_SATSUMA == 1
        return std::move(_satsuma_preprocessor->extractPreprocessedFormula());
#else
        return std::move(_kissat->extractPreprocessedFormula());
#endif
    }


    // Interrupt any preprocessing, no more need for a result
    void interrupt() {
        if (!_run_satsuma || _chain_kissat_after_satsuma) {
            _kissat_interrupted = true;
            if (_kissat_initialized) _kissat->interrupt();
        }
        if (_lingeling) _lingeling->interrupt();
        // TODO satsuma has no interrupt as of yet
        //if (_satsuma_preprocessor) _satsuma_preprocessor->interrupt();
    }
    void join(bool onlyWaitForModel) {
        if (!onlyWaitForModel && _fut_lingeling.valid()) _fut_lingeling.get(); // wait for solver thread to return
        if (_fut_kissat.valid()) _fut_kissat.get(); // wait for solver thread to return
        if (_fut_satsuma.valid()) _fut_satsuma.get();
    }

    void reconstructSolution(std::vector<int>& solution) {
        if (!_run_satsuma) {
            _kissat->reconstructSolutionFromPreprocessing(solution);
        } else {
            if (_chain_kissat_after_satsuma) {
                _kissat->reconstructSolutionFromPreprocessing(solution);
                solution.resize(numberOfVariables + 1);
            } else {
                solution.resize(numberOfVariables + 1);
            }
        }

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

    void loadFormulaFromExtracted(PortfolioSolverInterface* slv, const std::vector<int>& formula) {
        for (int lit : formula) {
            slv->addLiteral(lit);
        }
        slv->diversify(0);
    }

#if MALLOB_USE_SATSUMA == 1
	void loadFormulaToCnf2wl(satsuma::ICnf2wl& result){
		SerializedFormulaParser parser(Logger::getMainInstance(), _desc.getFormulaPayload(0), _desc.getFormulaPayloadSize(0));
        if (_params.compressFormula()) parser.setCompressed();
		numberOfVariables = _desc.getAppConfiguration().fixedSizeEntryToInt("__NV");
        int numberOfClauses = _desc.getAppConfiguration().fixedSizeEntryToInt("__NC");

        assert(numberOfVariables > 0 && numberOfVariables < 1000000000);
		result.reserve(numberOfVariables, numberOfClauses);
		int lit;
		std::vector<int> construct_clause;
		while (parser.getNextLiteral(lit)) {
			if (lit != 0) {
				construct_clause.push_back(lit);
			} else {
				result.add_clause(construct_clause); 

				construct_clause.clear();
			}
		}
	}
#endif
};

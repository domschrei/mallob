
#include "engine.hpp"

#include "../sharing/sharing_manager.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/data/portfolio_sequence.hpp"
#include "app/sat/data/revision_data.hpp"
#include "app/sat/data/theories/theory_specification.hpp"
#include "app/sat/sharing/clause_logger.hpp"
#include "app/sat/solvers/solver_portfolio_config.hpp"
#include "app/sat/solvers/solving_replay.hpp"
#include "util/logger.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/sys/timer.hpp"
#include "data/app_configuration.hpp"
#if MALLOB_USE_CADICAL
#include "../solvers/cadical.hpp"
#endif
#if MALLOB_USE_LINGELING
#include "../solvers/lingeling.hpp"
#endif
#if MALLOB_USE_KISSAT
#include "../solvers/kissat.hpp"
#endif
#include "app/sat/data/clause_histogram.hpp"
#include "app/sat/data/definitions.hpp"
#include "app/sat/data/sharing_statistics.hpp"
#include "app/sat/data/solver_statistics.hpp"
#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/execution/solver_thread.hpp"
#include "app/sat/execution/solving_state.hpp"
#include "app/sat/job/sat_process_config.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "util/option.hpp"
#include <climits>
#include <fstream>
#include <iterator>

class LratConnector;
#if MALLOB_USE_MINISAT
#include "../solvers/minisat.hpp"
#endif
#if MALLOB_USE_MERGESAT
#include "../solvers/mergesat.hpp"
#endif
#if MALLOB_USE_GLUCOSE
#include "../solvers/glucose.hpp"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <algorithm>
#include <csignal>
#include <cmath>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>

using namespace SolvingStates;

SatEngine::SatEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface) : 
			_params(params), _config(config), _logger(loggingInterface), _prefilter(params), _state(INITIALIZING) {

    int appRank = config.apprank;

	LOGGER(_logger, V4_VVER, "SAT engine for %s\n", config.getJobStr().c_str());
	//params.printParams();
	_num_solvers = config.threads;
	_num_active_solvers = _num_solvers;
	int numOrigSolvers = params.numThreadsPerProcess();
	_job_id = config.jobid;
	
	_block_result = _params.deterministicSolving();

	// Retrieve the string defining the cycle of solver choices, one character per solver
	// e.g. "llgc" => lingeling lingeling glucose cadical lingeling lingeling glucose ...
	std::string solverChoicesStr = params.satSolverSequence();
	PortfolioSequence portfolio;
	bool ok = portfolio.parse(solverChoicesStr);
	if (!ok) {
		LOG(V0_CRIT, "[ERROR] Invalid portfolio specified!\n");
		abort();
	}
	std::string proofDirectory;

	_spc.parseFromDirsAndFiles(params.satConfigDirs(), params.satConfigFiles());
	LOG(V3_VERB, "Parsed %i solver configuration rules\n", _spc.ruleCount());

	// Launched in some certified UNSAT mode?
    if (_params.proofOutputFile.isSet() || _params.onTheFlyChecking() || _params.palRup()) {

		// Override options
		if (!portfolio.featuresProofOutput()) {
			LOG(V2_INFO, "Certified UNSAT mode: Overriding portfolio to have all CaDiCaLs produce proofs\n");
			for (auto set : {&portfolio.prefix, &portfolio.cycle})
				for (auto& item : *set)
					if (item.baseSolver == PortfolioSequence::CADICAL)
						item.outputProof = true;
			if (!portfolio.featuresProofOutput()) {
				LOG(V0_CRIT, "[ERROR] No specified solver capable of producing proofs!\n");
				abort();
			}
		}
		ClauseMetadata::enableClauseIds();
		if (_params.onTheFlyChecking()) {
			ClauseMetadata::enableClauseSignatures();
			if (_params.onTheFlyCheckIncremental())
				ClauseMetadata::enableIncrementalSignatures();
		}

		if (_params.proofOutputFile.isSet() || _params.palRup()) {
			// Create directory for partial proofs
			proofDirectory = params.proofDirectory() + "/proof" + config.getJobStr();
			FileUtils::mkdir(proofDirectory);
			if (_params.compressFormula()) {
				LOG(V1_WARN, "[WARN] Using proof production (-proof) combined with formula compression (-cf): "
					"Irredundant clause IDs in the produced proof will be ordered differently (by clause length). "
					"Running a vanilla LRUP checker on the input CNF and the produced proof will fail!\n");
			}
			if (_params.palRup())
				LOG(V2_INFO, "Writing PalRUP proof to %s\n", proofDirectory.c_str());
		}
    }

	// Launched for deterministic solving?
	if (_params.deterministicSolving()) {
		if (_params.skipClauseSharingDiagonally()) {
			LOG(V3_VERB, "Deterministic mode: Disabling -scsd\n");
			_params.skipClauseSharingDiagonally.set(false);
		}
	}

	// Read options from app config
	AppConfiguration appConfig; appConfig.deserialize(params.applicationConfiguration());
	const std::string keyPrefix = "div-offset-prefix";
	const std::string keyCycle = "div-offset-cycle";
	const int divOffsetPrefix = appConfig.map.count(keyPrefix) ? atoi(appConfig.map[keyPrefix].c_str()) : 0;
	const int divOffsetCycle = appConfig.map.count(keyCycle) ? atoi(appConfig.map[keyCycle].c_str()) : 0;
	// Read # clauses and # vars from app config
	int numClauses, numVars, epochOffset, epochModulus;
	std::vector<std::pair<int*, std::string>> fields {
		{&numClauses, "__NC"},
		{&numVars, "__NV"},
		{&epochOffset, "__EO"},
		{&epochModulus, "__EM"},
	};
	for (auto [out, id] : fields) {
		if (!appConfig.map.count(id)) continue;
		std::string str = appConfig.map[id];
		while (str[str.size()-1] == '.') 
			str.resize(str.size()-1);
		*out = atoi(str.c_str());
		assert(*out >= 0 || log_return_false("[ERROR] illegal argument for app config key %s\n", id.c_str()));
	}

	if (appConfig.map.count("__OBJ")) {
		TheorySpecification spec(appConfig.map.at("__OBJ"));
		assert(spec.getRuleset().size() == 1);
		assert(spec.getRuleset()[0].type == IntegerRule::MINIMIZE);
		const IntegerTerm& sum = spec.getRuleset()[0].term1;
		assert(sum.type == IntegerTerm::ADD || sum.type == IntegerTerm::BIG_ADD);
		for (auto& prod : sum.children()) {
			assert(prod.type == IntegerTerm::MULTIPLY);
			assert(prod.children().size() == 2);
			assert(prod.children()[0].type == IntegerTerm::CONSTANT);
			long weight = prod.children()[0].inner();
			assert(prod.children()[1].type == IntegerTerm::LITERAL);
			int literal = prod.children()[1].inner();
			_objective.push_back({weight, literal});
		}
		LOGGER(_logger, V4_VVER, "Parsed objective\n");
	}

	// These numbers become the diversifier indices of the solvers on this node
	int numLgl = 0;
	int numGlu = 0;
	int numCdc = 0;
	int numMrg = 0;
	int numMini = 0;
	int numKis = 0;
	int numPre = 0;

	// Add solvers from full cycles on previous ranks
	// and from the begun cycle on the previous rank
	int numFullCycles = std::max(0, appRank * numOrigSolvers - (int)portfolio.prefix.size()) / portfolio.cycle.size();
	int begunCyclePos = std::max(0, appRank * numOrigSolvers - (int)portfolio.prefix.size()) % portfolio.cycle.size();
	bool hasPseudoincrementalSolvers = false;
	for (size_t i = 0; i < portfolio.cycle.size(); i++) {
		int* solverToAdd;
		bool pseudoIncremental = !portfolio.cycle[i].incremental;
		if (pseudoIncremental) hasPseudoincrementalSolvers = true;
		switch (portfolio.cycle[i].baseSolver) {
		case PortfolioSequence::LINGELING: solverToAdd = &numLgl; break;
		case PortfolioSequence::GLUCOSE: solverToAdd = &numGlu; break;
		case PortfolioSequence::CADICAL: solverToAdd = &numCdc; break;
		case PortfolioSequence::MERGESAT: solverToAdd = &numMrg; break;
		case PortfolioSequence::MINISAT: solverToAdd = &numMini; break;
		case PortfolioSequence::KISSAT: solverToAdd = &numKis; break;
		case PortfolioSequence::PREPROCESSOR: solverToAdd = &numPre; break;
		}
		*solverToAdd += numFullCycles + (i < begunCyclePos);
	}
	if (config.incremental && hasPseudoincrementalSolvers) {
		LOG(V0_CRIT, "[ERROR] Non-incremental solvers are currently unsupported for incremental jobs.\n");
		abort();
	}

	// Solver-agnostic options each solver in the portfolio will receive
	SolverSetup setup;
	setup.logger = &_logger;
	setup.jobname = config.getJobStr();
	setup.jobId = config.jobid;
	setup.baseSeed = params.seed();
	setup.isJobIncremental = config.incremental;
	setup.replayMode = _params.replay() == 1 ? SolvingReplay::RECORD :
		(_params.replay() == 2 ? SolvingReplay::REPLAY : SolvingReplay::NONE);
	setup.strictMaxLitsPerClause = params.strictClauseLengthLimit();
	setup.strictLbdLimit = params.strictLbdLimit();
	setup.qualityMaxLitsPerClause = params.qualityClauseLengthLimit();
	setup.qualityLbdLimit = params.qualityLbdLimit();
	setup.freeMaxLitsPerClause = params.freeClauseLengthLimit();
	setup.clauseBaseBufferSize = params.exportVolumePerThread() * _num_solvers;
	setup.anticipatedLitsToImportPerCycle = config.maxBroadcastedLitsPerCycle;
	setup.resetLbdBeforeImport = params.resetLbd() == MALLOB_RESET_LBD_AT_IMPORT;
	setup.incrementLbdBeforeImport = params.incrementLbd();
	setup.randomizeLbdBeforeImport = params.randomizeLbd();
	setup.hasPseudoincrementalSolvers = setup.isJobIncremental && hasPseudoincrementalSolvers;
	setup.solverRevision = 0;
	setup.minImportChunksPerSolver = params.minNumChunksForImportPerSolver();
	setup.numBufferedClsGenerations = params.bufferedImportedClsGenerations();
	setup.skipClauseSharingDiagonally = params.skipClauseSharingDiagonally();
	setup.adaptiveImportManager = params.adaptiveImportManager();
	setup.maxNumSolvers = config.mpisize * params.numThreadsPerProcess();
	setup.numVars = numVars;
	setup.numOriginalClauses = numClauses;
	int sqrt = std::ceil(std::sqrt((double) setup.maxNumSolvers));
	setup.proofDir = proofDirectory;
	if (_params.proofCompressionMode() == 1) setup.compressProofMode = SolverSetup::XZ;
	if (_params.proofCompressionMode() == 2) setup.compressProofMode = SolverSetup::VASKIN_GOETZ;

	LratConnector* modelCheckingLratConnector {nullptr};
	setup.nbSkippedIdEpochs = std::max(0, epochOffset + epochModulus * config.nbPreviousBalancingEpochs);
	if (params.satProfilingLevel() >= 0) {
		setup.profilingBaseDir = params.satProfilingDir();
		if (setup.profilingBaseDir.empty()) setup.profilingBaseDir = TmpDir::getGeneralTmpDir();
		setup.profilingBaseDir += "/" + std::to_string(appRank) + "/";
		FileUtils::mkdir(setup.profilingBaseDir);
		setup.profilingLevel = params.satProfilingLevel();
	}
	setup.memoryFactor = numOrigSolvers / (float)_num_active_solvers;
	setup.objectiveFunction = _objective;

	// Pre-create PalRUP proof directories for *all* solver IDs, including cancelled ones.
	if (params.palRup()) for (setup.localId = 0; setup.localId < numOrigSolvers; setup.localId++) {
		setup.globalId = appRank * numOrigSolvers + setup.localId;
		int sqrt = std::ceil(std::sqrt((double) setup.maxNumSolvers));
		auto dir = proofDirectory + "/" + std::to_string((int)(setup.globalId / sqrt)) + "/" + std::to_string(setup.globalId);
		LOG(V4_VVER, "MKDIR %s\n", dir.c_str());
		FileUtils::mkdir(dir);
	}

	// Instantiate solvers according to the global solver IDs and diversification indices
	int cyclePos = begunCyclePos;
	for (setup.localId = 0; setup.localId < _num_solvers; setup.localId++) {
		setup.globalId = appRank * numOrigSolvers + setup.localId;

		// Which solver? Which diversification?
		PortfolioSequence::Item item;
		if (setup.globalId < portfolio.prefix.size()) {
			// This solver belongs to the specified prefix
			item = portfolio.prefix[setup.globalId];
			const int nbBefore = std::count_if(portfolio.prefix.begin(),
				portfolio.prefix.begin()+setup.globalId,
				[&](auto& x) {return x.baseSolver == item.baseSolver;});
			setup.diversificationIndex = nbBefore + divOffsetPrefix;
		} else {
			item = portfolio.cycle[cyclePos];
			switch (item.baseSolver) {
			case PortfolioSequence::LINGELING: setup.diversificationIndex = numLgl++; break;
			case PortfolioSequence::CADICAL: setup.diversificationIndex = numCdc++; break;
			case PortfolioSequence::MERGESAT: setup.diversificationIndex = numMrg++; break;
			case PortfolioSequence::MINISAT: setup.diversificationIndex = numMini++; break;
			case PortfolioSequence::GLUCOSE: setup.diversificationIndex = numGlu++; break;
			case PortfolioSequence::KISSAT: setup.diversificationIndex = numKis++; break;
			case PortfolioSequence::PREPROCESSOR: setup.diversificationIndex = numPre++; break;
			}
			setup.diversificationIndex += divOffsetCycle;
		}
		setup.solverType = item.baseSolver;
		setup.flavour = item.flavour;
		setup.doIncrementalSolving = (setup.isJobIncremental || params.enumerateModels()) && item.incremental;
		setup.certifiedUnsat = item.outputProof && (params.proofOutputFile.isSet() || params.onTheFlyChecking() || _params.palRup());
		setup.onTheFlyChecking = setup.certifiedUnsat && params.onTheFlyChecking();
		setup.onTheFlyCheckModel = params.onTheFlyChecking() && params.onTheFlyCheckModel();
		setup.incrementalImpCheck = params.onTheFlyCheckIncremental();
		setup.usePalRupFormat = params.palRup();
		setup.outputBinaryPalRup = params.palRupBinary();
		setup.trustedParserForced = params.forceIncrementalTrustedParser();
		setup.modelCheckingLratConnector = modelCheckingLratConnector;
		setup.avoidUnsatParticipation = (params.proofOutputFile.isSet() || params.onTheFlyChecking() || _params.palRup()) && !item.outputProof;
		setup.exportClauses = !setup.avoidUnsatParticipation;
		setup.enumerateModels = params.enumerateModels();
		setup.solverConfig = &_spc;

		_solver_interfaces.push_back(createSolver(setup));
		cyclePos = (cyclePos+1) % portfolio.cycle.size();
		auto mclc = _solver_interfaces.back()->getSolverSetup().modelCheckingLratConnector;
		if (mclc) modelCheckingLratConnector = mclc;
	}
	_base_solver_setup = setup;

	_sharing_manager.reset(new SharingManager(_solver_interfaces, _params, _logger, 
		/*max. deferred literals per solver=*/5*config.maxBroadcastedLitsPerCycle, config.apprank));
	_sharing_manager->setClausePrefilter(_prefilter);
	LOGGER(_logger, V5_DEBG, "initialized\n");
}

std::shared_ptr<PortfolioSolverInterface> SatEngine::createSolver(const SolverSetup& setup) {
	std::shared_ptr<PortfolioSolverInterface> solver;
	switch (setup.solverType) {
#if MALLOB_USE_LINGELING
	case 'l':
	case 'L':
		// Lingeling
		LOGGER(_logger, V4_VVER, "S%i : Lingeling-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new Lingeling(setup));
		break;
#endif
#if MALLOB_USE_CADICAL
	case 'c':
	case 'C':
		// Cadical
		LOGGER(_logger, V4_VVER, "S%i : Cadical-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new Cadical(setup));
		break;
#endif
#if MALLOB_USE_KISSAT
	case 'k':
	case 'v': // variable addition via Kissat
	case 'p': // preprocessing via Kissat
		// Kissat
		LOGGER(_logger, V4_VVER, "S%i : Kissat%s%s-%i\n", setup.globalId,
			setup.solverType == 'v' ? "-BVA": "",
			setup.solverType == 'p' ? "-pre": "",
			setup.diversificationIndex);
		solver.reset(new Kissat(setup));
		break;
#endif
#ifdef MALLOB_USE_MERGESAT
	case 'm':
	//case 'M': // no support for incremental mode as of now
		// MergeSat
		LOGGER(_logger, V4_VVER, "S%i : MergeSat-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new MergeSatBackend(setup));
		break;
#endif
#ifdef MALLOB_USE_GLUCOSE
	case 'g':
	case 'G':
		// Glucose
		LOGGER(_logger, V4_VVER, "S%i: Glucose-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new MGlucose(setup));
		break;
#endif
#ifdef MALLOB_USE_MINISAT
	case 'n':
	case 'N':
		// Minisat
		LOGGER(_logger, V4_VVER, "S%i: Minisat-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new MiniSat(setup));
		break;
#endif
	default:
		// Invalid solver
		LOGGER(_logger, V0_CRIT, "[ERROR] Invalid solver \"%c\" assigned\n", setup.solverType);
		_logger.flush();
		abort();
		break;
	}
	return solver;
}

void SatEngine::appendRevision(int revision, RevisionData data, bool lastRevisionForNow) {
	
	LOGGER(_logger, V4_VVER, "Import rev. %i: size %lu\n", revision, data.fLits->size());
	assert(_revision+1 == revision);
	_revision_data.push_back(data);
	_sharing_manager->setImportedRevision(revision);
	_prefilter.notifyFormula(data.fLits->data(), data.fLits->size());

	// since we enter a new revision, output the solutions enumerated in the last one
	writeEnumeratedSolutions();

	for (size_t i = 0; i < _num_active_solvers; i++) {
		if (revision == 0) {
			// Initialize solver thread
			_solver_threads.emplace_back(new SolverThread(
				_params, _config, _solver_interfaces[i], data, i
			));
		} else {
			if (_solver_interfaces[i]->getSolverSetup().doIncrementalSolving) {
				LOGGER(_logger, V4_VVER, "Solver %i is incremental: forward next revision\n", i);
				// True incremental SAT solving
				_solver_threads[i]->appendRevision(revision, data);
			} else {
				LOGGER(_logger, V4_VVER, "Solver %i is non-incremental: phase out\n", i);
				if (!lastRevisionForNow) {
					// Another revision will be imported momentarily: 
					// Wait with restarting a whole new solver thread
					continue;
				}
				if (_solvers_started && _params.abortNonincrementalSubprocess()) {
					// Non-incremental solver being "restarted" with a new revision:
					// Abort (but do not create a thread trace) such that a new, fresh
					// process will be initialized
					LOGGER(_logger, V3_VERB, "Restarting this non-incremental subprocess\n");
					raise(SIGUSR2);
				}
				// Pseudo-incremental SAT solving: 
				// Phase out old solver thread
				resetSolver(i);
				// Load entire formula 
				for (int importedRevision = 0; importedRevision <= revision; importedRevision++) {
					auto data = _revision_data[importedRevision];
					_solver_threads[i]->appendRevision(importedRevision, data);
				}
				_sharing_manager->continueClauseImport(i);
				if (_solvers_started) _solver_threads[i]->start();
				_num_active_solvers++; // decremented in resetSolver(i)
			}
		}
	}
	if (!_params.enumerateModels())
		_revision_data.back().fLits.reset(); // formula increment no longer needed here
	_revision = revision;
}

void SatEngine::solve() {
	assert(_revision >= 0);
	_result.result = UNKNOWN;
	if (!_solvers_started) {
		// Need to start threads
		LOGGER(_logger, V4_VVER, "starting threads\n");
		for (size_t i = 0; i < std::min(_num_active_solvers, _solver_threads.size()); i++)
			_solver_threads[i]->start();
		_solvers_started = true;

		if (_params.injectProofData.isSet()) {
			// Read proof meta data to inject into solving
			{
				std::ifstream ifs(_params.injectProofData() + "/winning-id.txt");
				std::string content( (std::istreambuf_iterator<char>(ifs)),
					(std::istreambuf_iterator<char>()));
				_result.winningInstanceId = atoi(content.c_str());
			}
			{
				std::ifstream ifs(_params.injectProofData() + "/global-start-of-success-epoch.txt");
				std::string content( (std::istreambuf_iterator<char>(ifs)),
					(std::istreambuf_iterator<char>()));
				_result.globalStartOfSuccessEpoch = atol(content.c_str());
			}
		}
	}
	_state = ACTIVE;
}

bool SatEngine::isFullyInitialized() {
	if (_state == INITIALIZING) return false;
	for (size_t i = 0; i < std::min(_num_active_solvers, _solver_threads.size()); i++) {
		if (!_solver_threads[i]->isInitialized()) return false;
	}
	return true;
}

int SatEngine::solveLoop() {
	if (isCleanedUp()) return -1;
	if (_block_result) return -1;

	// perform GC in export filter whenever necessary
	if (_sharing_manager) _sharing_manager->collectGarbageInFilter();

	bool done = false;
	bool preprocessingResult = false;

	// Termination condition for model enumeration when sufficiently many models have been found
	if (!_enumerated_solutions.empty() && _nb_globally_found_models >= _params.enumerationLimit()) {
		assert(_result.result == SAT);
		done = true;
	}
	// Termination condition for model enumeration when the time limit has elapsed
	if (!_enumerated_solutions.empty() && _time_of_first_model >= 0 &&
		Timer::elapsedSeconds() - _time_of_first_model >= _params.enumerationTimeLimit()) {
		assert(_result.result == SAT);
		done = true;
	}

	// Solving done?
	if (!done) for (size_t i = 0; i < std::min(_num_active_solvers, _solver_threads.size()); i++) {
		if (_params.injectProofData.isSet()) {
			// Inject solver result from the specified proof meta data
			if (_solver_interfaces[i]->getGlobalId() == _result.winningInstanceId) {
				done = true;
				_result.result = UNSAT;
				_result.revision = 0;
				break;
			}
		} else if (_solver_threads[i]->hasFoundResult(_revision)) {

			if (_params.deterministicSolving() && _solver_interfaces[i]->getGlobalId() != _winning_solver_id)
				continue; // not the successful solver we're looking for

			auto& result = _solver_threads[i]->getSatResult();
			if (result.result > 0 && result.revision == _revision) {

				// only override previous result if the new one isn't an UNSAT result from an enumerating thread
				if (result.result == SAT || !_solver_interfaces[i]->getSolverSetup().currentlyEnumerating)
					_result = std::move(result);
				_result.winningInstanceId = _solver_interfaces[i]->getGlobalId();
				_result.globalStartOfSuccessEpoch = _sharing_manager->getGlobalStartOfSuccessEpoch();

				if (result.result == SAT && _params.enumerateModels()) {
					// model enumeration: try next model
					// Reset solver thread
					if (_solver_interfaces[i]->getSolverSetup().doIncrementalSolving) {
						// incremental solver is *done* enumerating (since it internally loops exhaustively)
						// Note: we don't need to lock the enumerated solutions since the solver already returned
						auto solutions = std::move(_solver_threads[i]->extractEnumeratedSolutions());
						size_t size = _enumerated_solutions.size();
						_enumerated_solutions.insert(solutions.begin(), solutions.end());
						if (_enumerated_solutions.size() > size) {
							LOGGER(_logger, V3_VERB, "S%i found %lu new solutions (process-locally now %lu)\n",
								i, _enumerated_solutions.size()-size, _enumerated_solutions.size());
						}
						done = true; // actually report a solution
					} else {
						// compress model and try to insert it into unique set of models
						auto sol = _result.copySolution();
						auto [it, success] = _enumerated_solutions.insert(ModelStringCompressor::compress(sol));
						if (success) {
							// found model is new
							LOGGER(_logger, V4_VVER, "S%i found new solution (process-locally no. %lu)\n", i, _enumerated_solutions.size());
							// add its blocking clause
							for (int pos = 1; pos < sol.size(); pos++) {
								_enumeration_blocking_clauses.push_back(-sol[pos]);
							}
							_enumeration_blocking_clauses.push_back(0);
						}
						// add unit clauses found thus far to clause set (temporarily)
						int sizeBefore = _enumeration_blocking_clauses.size();
						if (_params.enumerationFeedUnits() && _sharing_manager->getUnitClauseLogger()) {
							const auto& units = _sharing_manager->getUnitClauseLogger()->beginAccessingPublishedClauses();
							for (auto& cls : units) {
								_enumeration_blocking_clauses.push_back(cls.back());
								_enumeration_blocking_clauses.push_back(0);
							}
							LOG(V4_VVER, "S%i feeding %i units\n", i, units.size());
						}
						// reset non-incremental solver with added blocking clauses (plus units)
						resetSolver(i, &_enumeration_blocking_clauses);
						// re-remove units, return access to published units
						_enumeration_blocking_clauses.resize(sizeBefore);
						if (_sharing_manager->getUnitClauseLogger())
							_sharing_manager->getUnitClauseLogger()->endAccessingPublishedClauses();
						// append formula revisions to solver
						for (int importedRevision = 0; importedRevision <= _revision; importedRevision++) {
							auto data = _revision_data[importedRevision];
							_solver_threads[i]->appendRevision(importedRevision, data);
						}
						// start solver again
						_sharing_manager->continueClauseImport(i);
						_solver_threads[i]->start();
						_num_active_solvers++; // decremented in resetSolver(i)
					}

				} else {
					done = true;
				}
				break;
			}

		} else if (_params.enumerateModels() && _solver_interfaces[i]->getSolverSetup().doIncrementalSolving
				&& _solver_threads[i]->getActiveRevision() == _revision) {
			// An incremental solver thread's enumeration is still ongoing, but we want to report
			// the models found thus far directly (also to watch limits etc.) without concluding the revision.

			_solver_threads[i]->lockEnumeratedSolutions();
			auto solutions = std::move(_solver_threads[i]->extractEnumeratedSolutions());
			_solver_threads[i]->unlockEnumeratedSolutions();
			size_t size = _enumerated_solutions.size();
			_enumerated_solutions.insert(solutions.begin(), solutions.end());
			if (_enumerated_solutions.size() > size) {
				LOGGER(_logger, V3_VERB, "S%i found %lu new solutions (process-locally now %lu)\n",
					i, _enumerated_solutions.size()-size, _enumerated_solutions.size());
				if (_result.result != SAT) {
					// prematurely create a suitable SatResult object
					_result.revision = _revision;
					_result.result = SAT;
					_result.winningInstanceId = _solver_interfaces[i]->getGlobalId();
					_result.globalStartOfSuccessEpoch = _sharing_manager->getGlobalStartOfSuccessEpoch();
					auto primarySol = ModelStringCompressor::decompress(*_enumerated_solutions.begin());
					_result.setSolutionToSerialize(primarySol.data(), primarySol.size());
				}
			}
		}
		if (!preprocessingResult && _solver_threads[i]->hasPreprocessedFormula()) {
			_preprocessed_formula = std::move(_solver_threads[i]->extractPreprocessedFormula());
			preprocessingResult = true;
		}
	}

	if (done) {
		LOGGER(_logger, V6_DEBGV, "Returning result\n");
		return _result.result;
	} else if (preprocessingResult) return 99;
    return -1; // no result yet
}

bool SatEngine::isReadyToPrepareSharing() const {
	// If certified UNSAT is enabled, no sharing operation can be ongoing
	// (otherwise, this op must be finished first, for clause ID consistency)
	return !_params.proofOutputFile.isSet() || !_sharing_manager->isSharingOperationOngoing();
}

void SatEngine::setClauseBufferRevision(int revision) {
	if (isCleanedUp()) return;
 	LOGGER(_logger, V5_DEBG, "set clause rev=%i\n", revision);
	_sharing_manager->setImportedRevision(revision);
}

void SatEngine::updateBestFoundObjectiveCost(long long bestFoundObjectiveCost) {
	if (isCleanedUp()) return;
	if (bestFoundObjectiveCost != LLONG_MAX)
		LOGGER(_logger, V4_VVER, "update best found objective cost: %lld\n", bestFoundObjectiveCost);
	for (size_t i = 0; i < _num_active_solvers; i++) {
		if (_solver_interfaces[i] && _solver_interfaces[i]->getOptimizer())
			_solver_interfaces[i]->getOptimizer()->update_best_found_objective_cost(bestFoundObjectiveCost);
	}
}

std::vector<int> SatEngine::prepareSharing(int literalLimit, int& outSuccessfulSolverId, int& outNbLits) {
	if (isCleanedUp()) return std::vector<int>(2); // checksum, nothing else
	LOGGER(_logger, V5_DEBG, "collecting clauses on this node\n");
	return _sharing_manager->prepareSharing(literalLimit, outSuccessfulSolverId, outNbLits);
}

std::vector<int> SatEngine::filterSharing(std::vector<int>& clauseBuf) {
	if (isCleanedUp()) return std::vector<int>();
	return _sharing_manager->filterSharing(clauseBuf);
}

void SatEngine::addSharingEpoch(int epoch) {
	if (isCleanedUp()) return;
	_sharing_manager->addSharingEpoch(epoch);
}

void SatEngine::digestSharingWithFilter(std::vector<int>& clauseBuf, std::vector<int>& filter) {
	if (isCleanedUp()) return;
	_sharing_manager->digestSharingWithFilter(clauseBuf, &filter);
}

void SatEngine::digestSharingWithoutFilter(std::vector<int>& clauseBuf, bool stateless) {
	if (isCleanedUp()) return;
	_sharing_manager->digestSharingWithoutFilter(clauseBuf, stateless);
}

void SatEngine::returnClauses(std::vector<int>& clauseBuf) {
	if (isCleanedUp()) return;
	_sharing_manager->returnClauses(clauseBuf);
}

void SatEngine::digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>& clauseBuf) {
	if (isCleanedUp()) return;
	_sharing_manager->digestHistoricClauses(epochBegin, epochEnd, clauseBuf);
}

void SatEngine::syncDeterministicSolvingAndCheckForLocalWinner() {
	if (_block_result) {
		_block_result = !_sharing_manager->syncDeterministicSolvingAndCheckForWinningSolver();
	}
}

void SatEngine::reduceActiveThreadCount() {
	// Reduce thread count by 10% (but at least one thread)
	int nbThreads = (int)_num_active_solvers - (int)std::max(1UL, (size_t)std::round(0.1*_num_active_solvers));
	setActiveThreadCount(nbThreads);
}

void SatEngine::setActiveThreadCount(int nbThreads) {
	if (nbThreads < 1) return;
	int nbThreadsToTerminate = (int)_num_active_solvers - nbThreads;
	if (nbThreadsToTerminate <= 0) return;
	for (int termIdx = 0; termIdx < nbThreadsToTerminate; termIdx++) {
		size_t i = _num_active_solvers-1;
		resetSolver(i);
	}
}

void SatEngine::resetSolver(int i, const std::vector<int>* addedLits) {
	LOGGER(_logger, V3_VERB, "Terminating local solver %lu\n", i);
	assert(i >= 0 && i < _solver_interfaces.size());
	_sharing_manager->stopClauseImport(i);
	SolverSetup s = _solver_interfaces[i]->getSolverSetup();
	s.solverRevision++;
	if (addedLits) s.inlineLiterals = *addedLits;
	_solver_threads[i]->setTerminate(false);
	auto movedSolver = std::move(_solver_interfaces[i]);
	_solver_interfaces[i] = createSolver(s);
	auto movedThread = std::move(_solver_threads[i]);
	_solver_threads[i] = std::shared_ptr<SolverThread>(new SolverThread(
		_params, _config, _solver_interfaces[i], {}, i
	));
	_num_active_solvers--;
	_solver_thread_cleanups.push_back(ProcessWideThreadPool::get().addTask([thread = std::move(movedThread), solver = std::move(movedSolver)]() mutable {
		thread->cleanUpAsynchronously();
		thread->tryJoin();
		thread.reset();
		solver.reset();
	}));
}

void SatEngine::dumpStats(bool final) {
	if (isCleanedUp() || !isFullyInitialized()) return;

	int verb = final ? V2_INFO : V4_VVER;

	// Solver statistics
	SolverStatistics solveStats;
	for (size_t i = 0; i < _num_solvers; i++) {
		SolverStatistics st = _solver_interfaces[i]->getSolverStats();
		int globalId = _solver_interfaces[i]->getGlobalId();
		int sVerb = final ? V3_VERB : verb;
		_logger.log(sVerb, "%sS%d %s\n",
				final ? "END " : "", globalId, st.getReport().c_str());
		_logger.log(sVerb, "%sS%d clenhist prod %s\n",
				final ? "END " : "", globalId, st.histProduced->getReport().c_str());
		_logger.log(sVerb, "%sS%d clenhist digd %s\n",
				final ? "END " : "", globalId, st.histDigested->getReport().c_str());
		solveStats.aggregate(st);
	}
	_logger.log(verb, "%s%s\n", final ? "END " : "", solveStats.getReport().c_str());

	// Sharing statistics
	SharingStatistics shareStats;
	if (_sharing_manager != NULL) shareStats = _sharing_manager->getStatistics();
	_logger.log(shareStats.empty() ? V4_VVER : verb, "%s%s\n",
		final ? "END " : "", shareStats.getReport().c_str());

	if (final) {
		std::initializer_list<std::pair<std::string, ClauseHistogram*>> histList = {
			{"prod", shareStats.histProduced},
			{"flfl", shareStats.histFailedFilter},
			{"admt", shareStats.histAdmittedToDb},
			{"drpd", shareStats.histDroppedBeforeDb},
			{"dltd", shareStats.histDeletedInSlots},
			{"retd", shareStats.histReturnedToDb}
		};
		// Histogram over clause lengths (do not print trailing zeroes)
		for (auto& [name, hist] : histList) {
			// Increased verbosity for empty histograms
			_logger.log(hist->getTotal()==0 ? V4_VVER : verb,
				"clenhist %s %s\n", name.c_str(), hist->getReport().c_str());
		}

		// Flush logs
		for (auto& solver : _solver_interfaces) solver->getLogger().flush();
		_logger.flush();
	}
}

void SatEngine::setWinningSolverId(int globalId) {
	_sharing_manager->setWinningSolverId(globalId);
	_winning_solver_id = globalId;
}

void SatEngine::terminateSolvers(bool hardTermination) {
	for (auto& solver : _solver_threads) {
		solver->setTerminate(hardTermination);
	}
}

SatEngine::LastAdmittedStats SatEngine::getLastAdmittedClauseShare() {
	return LastAdmittedStats {
		_sharing_manager->getLastNumAdmittedClausesToImport(), 
		_sharing_manager->getLastNumClausesToImport(),
		_sharing_manager->getLastNumAdmittedLitsToImport()
	};
}

long long SatEngine::getBestFoundObjectiveCost() const {
	long long best {LLONG_MAX};
	for (size_t i = 0; i < _num_active_solvers; i++) {
		if (_solver_interfaces[i] && _solver_interfaces[i]->getOptimizer())
			best = std::min(best, _solver_interfaces[i]->getOptimizer()->best_objective_found_so_far());
	}
	return best;
}

int SatEngine::getNbLocallyFoundModels() const {
	return _enumerated_solutions.size();
}
void SatEngine::updateNbGloballyFoundModels(int rev, int nbFound) {
	if (_revision != rev) return;
	if (nbFound > _nb_globally_found_models) {
		// log progress
		if (_config.apprank == 0)
			LOG(V5_DEBG, "nb found solutions: %i\n", nbFound);
		else
			LOG(V3_VERB, "nb found solutions: %i\n", nbFound);
	}
	_nb_globally_found_models = nbFound;
	if (_nb_globally_found_models > 0 && _time_of_first_model < 0)
		_time_of_first_model = Timer::elapsedSeconds();
}

void SatEngine::writeClauseEpochs() {
	if (_params.injectProofData.isSet()) return;
	std::string filename = _params.proofDirectory() + "/proof"
		+ _config.getJobStr() + "/clauseepochs." + std::to_string(_config.apprank);
	_sharing_manager->writeClauseEpochs(/*_solver_interfaces[0]->getSolverSetup().proofDir, 
		_solver_interfaces[0]->getGlobalId(), */filename);
}

void SatEngine::writeEnumeratedSolutions() {
	if (_enumerated_solutions.empty() || !_params.solutionToFile.isSet()) return;

	std::ofstream ofs(_params.solutionToFile() + ".#"
		+ std::to_string(_config.jobid) + ".enum."
		+ (_revision > 0 ? "rev" + std::to_string(_revision) + "." : "")
		+ std::to_string(_config.apprank));
	for (auto& sol : _enumerated_solutions)
		ofs << sol << std::endl;

	_enumerated_solutions.clear();
	_enumeration_blocking_clauses.clear();
	_nb_globally_found_models = 0;
	_time_of_first_model = -1;
}

void SatEngine::cleanUp(bool hardTermination) {
	double time = Timer::elapsedSeconds();

	LOGGER(_logger, V4_VVER, "[engine-cleanup] enter\n");

	_sharing_manager->finalize();
	writeEnumeratedSolutions();

	// Terminate any remaining running threads
	auto& setup = _base_solver_setup;
	terminateSolvers(hardTermination);
	if (hardTermination) {
		if (_params.proofOutputFile.isSet()) writeClauseEpochs();
		// Create (empty) proof files where none were created
		if (_params.palRup()) {
			int sqrt = std::ceil(std::sqrt((double) setup.maxNumSolvers));
			for (int localId = 0; localId < _params.numThreadsPerProcess(); localId++) {
				int globalId = _config.apprank * _params.numThreadsPerProcess() + localId;
				auto dir = setup.proofDir + "/" + std::to_string((int)(globalId / sqrt)) + "/" + std::to_string(globalId);
				if (FileUtils::exists(dir + "/out.palrup.xz") || FileUtils::exists(dir + "/out.palrup.vg"))
					continue;
				FileUtils::create(dir + "/out.palrup");
			}
		}
		LOGGER(_logger, V4_VVER, "[engine-cleanup] done - hard exit pending\n");
		return;
	}
	_cleaned_up = true;

	// join and delete threads
	for (auto& thread : _solver_threads) thread->tryJoin();
	for (auto& thread : _obsolete_solver_threads) thread->tryJoin();
	_solver_threads.clear();
	_obsolete_solver_threads.clear();
	for (auto& fut : _solver_thread_cleanups) fut.get(); // wait for cleanups
	_solver_thread_cleanups.clear();

	LOGGER(_logger, V5_DEBG, "[engine-cleanup] joined threads\n");

	// delete solvers in reverse order of how they were created
	for (int i = _solver_interfaces.size()-1; i >= 0; i--)
		_solver_interfaces[i].reset();
	_solver_interfaces.clear();
	LOGGER(_logger, V5_DEBG, "[engine-cleanup] cleared solvers\n");

	time = Timer::elapsedSeconds() - time;
	LOGGER(_logger, V4_VVER, "[engine-cleanup] done, took %.3f s\n", time);
	_logger.flush();

	if (_params.proofOutputFile.isSet()) writeClauseEpochs();
}

SatEngine::~SatEngine() {
	if (!_cleaned_up) {
		cleanUp();
	}
}

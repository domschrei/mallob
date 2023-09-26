
#include "engine.hpp"

#include "../sharing/sharing_manager.hpp"
#include "app/sat/data/clause.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "data/app_configuration.hpp"
#include "../solvers/cadical.hpp"
#include "../solvers/lingeling.hpp"
#include "../solvers/kissat.hpp"
#if MALLOB_USE_MERGESAT
#include "../solvers/mergesat.hpp"
#endif
#if MALLOB_USE_GLUCOSE
#include "../solvers/glucose.hpp"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <algorithm>
#include <csignal>
#include <unistd.h>
#include <sched.h>
#include <filesystem>
#include "util/assert.hpp"

using namespace SolvingStates;

SatEngine::SatEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface) : 
			_params(params), _config(config), _logger(loggingInterface), _state(INITIALIZING) {
	
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
	std::string solverChoices = params.satSolverSequence();
	std::string proofDirectory;

	// Launched in certified UNSAT mode?
    if (_params.certifiedUnsat()) {
		
		if (_params.inputShuffleProbability() > 0) {
			LOG(V3_VERB, "Certified UNSAT mode: Disabling input shuffling\n");
			_params.inputShuffleProbability.set(0);
		}

		// Override options
		if (solverChoices != "c") {
			LOG(V2_INFO, "Certified UNSAT mode: Overriding portfolio to non-incremental CaDiCaL only\n");
			solverChoices = "c";
		}
		ClauseMetadata::enableClauseIds();

		// Create directory for partial proofs
		std::filesystem::path base_dir(params.logDirectory());
		std::filesystem::path proof_dir("proof" + config.getJobStr());
		std::filesystem::path full_path = base_dir / proof_dir;
		create_directory(full_path);
		proofDirectory = full_path.string();
    }

	// Launched for deterministic solving?
	if (_params.deterministicSolving()) {
		if (_params.skipClauseSharingDiagonally()) {
			LOG(V3_VERB, "Deterministic mode: Disabling -scsd\n");
			_params.skipClauseSharingDiagonally.set(false);
		}
	}
	
	// These numbers become the diversifier indices of the solvers on this node
	int numLgl = 0;
	int numGlu = 0;
	int numCdc = 0;
	int numMrg = 0;
	int numKis = 0;

	// Read options from app config
	AppConfiguration appConfig; appConfig.deserialize(params.applicationConfiguration());
	std::string key = "diversification-offset";
	int diversificationOffset = appConfig.map.count(key) ? atoi(appConfig.map[key].c_str()) : 0;

	// Read # clauses and # vars from app config
	int numClauses, numVars;
	std::vector<std::pair<int*, std::string>> fields {
		{&numClauses, "__NC"},
		{&numVars, "__NV"}
	};
	for (auto [out, id] : fields) {
		std::string str = appConfig.map[id];
		while (str[str.size()-1] == '.') 
			str.resize(str.size()-1);
		*out = atoi(str.c_str());
		assert(*out > 0);
	}

	// Add solvers from full cycles on previous ranks
	// and from the begun cycle on the previous rank
	int numFullCycles = (appRank * numOrigSolvers) / solverChoices.size();
	int begunCyclePos = (appRank * numOrigSolvers) % solverChoices.size();
	bool hasPseudoincrementalSolvers = false;
	for (size_t i = 0; i < solverChoices.size(); i++) {
		int* solverToAdd;
		bool pseudoIncremental = islower(solverChoices[i]);
		if (pseudoIncremental) hasPseudoincrementalSolvers = true;
		switch (solverChoices[i]) {
		case 'l': case 'L': solverToAdd = &numLgl; break;
		case 'g': case 'G': solverToAdd = &numGlu; break;
		case 'c': case 'C': solverToAdd = &numCdc; break;
		case 'm': case 'M': solverToAdd = &numMrg; break;
		case 'k': case 'K': solverToAdd = &numKis; break;
		}
		*solverToAdd += numFullCycles + (i < begunCyclePos);
	}

	// Solver-agnostic options each solver in the portfolio will receive
	SolverSetup setup;
	setup.logger = &_logger;
	setup.jobname = config.getJobStr();
	setup.isJobIncremental = config.incremental;
	setup.strictClauseLengthLimit = params.strictClauseLengthLimit();
	setup.strictLbdLimit = params.strictLbdLimit();
	setup.qualityClauseLengthLimit = params.qualityClauseLengthLimit();
	setup.qualityLbdLimit = params.qualityLbdLimit();
	setup.clauseBaseBufferSize = params.clauseBufferBaseSize();
	setup.anticipatedLitsToImportPerCycle = config.maxBroadcastedLitsPerCycle;
	setup.resetLbdBeforeImport = params.resetLbd() == MALLOB_RESET_LBD_AT_IMPORT;
	setup.incrementLbdBeforeImport = params.incrementLbd();
	setup.hasPseudoincrementalSolvers = setup.isJobIncremental && hasPseudoincrementalSolvers;
	setup.solverRevision = 0;
	setup.minNumChunksPerSolver = params.minNumChunksForImportPerSolver();
	setup.numBufferedClsGenerations = params.bufferedImportedClsGenerations();
	setup.skipClauseSharingDiagonally = params.skipClauseSharingDiagonally();
	setup.diversifyNoise = params.diversifyNoise();
	setup.diversifyNative = params.diversifyNative();
	setup.diversifyFanOut = params.diversifyFanOut();
	setup.diversifyInitShuffle = params.diversifyInitShuffle();
	switch (_params.diversifyElimination()) {
	case 0:
		setup.eliminationSetting = SolverSetup::ALLOW_ALL;
		break;
	case 1:
		setup.eliminationSetting = SolverSetup::DISABLE_SOME;
		break;
	case 2:
		setup.eliminationSetting = SolverSetup::DISABLE_MOST;
		break;
	case 3:
		setup.eliminationSetting = SolverSetup::DISABLE_ALL;
		break;
	}
	setup.adaptiveImportManager = params.adaptiveImportManager();
	setup.certifiedUnsat = params.certifiedUnsat();
	setup.maxNumSolvers = config.mpisize * params.numThreadsPerProcess();
	setup.numVars = numVars;
	setup.numOriginalClauses = numClauses;
	setup.proofDir = proofDirectory;

	// Instantiate solvers according to the global solver IDs and diversification indices
	int cyclePos = begunCyclePos;
	for (setup.localId = 0; setup.localId < _num_solvers; setup.localId++) {
		setup.globalId = appRank * numOrigSolvers + setup.localId;
		// Which solver?
		setup.solverType = solverChoices[cyclePos];
		setup.doIncrementalSolving = setup.isJobIncremental && !islower(setup.solverType);
		switch (setup.solverType) {
		case 'l': case 'L': setup.diversificationIndex = numLgl++; break;
		case 'c': case 'C': setup.diversificationIndex = numCdc++; break;
		case 'm': case 'M': setup.diversificationIndex = numMrg++; break;
		case 'g': case 'G': setup.diversificationIndex = numGlu++; break;
		case 'k': case 'K': setup.diversificationIndex = numKis++; break;
		}
		setup.diversificationIndex += diversificationOffset;
		_solver_interfaces.emplace_back(createSolver(setup));
		cyclePos = (cyclePos+1) % solverChoices.size();
	}

	_sharing_manager.reset(new SharingManager(_solver_interfaces, _params, _logger, 
		/*max. deferred literals per solver=*/5*config.maxBroadcastedLitsPerCycle, config.apprank));
	LOGGER(_logger, V5_DEBG, "initialized\n");
}

std::shared_ptr<PortfolioSolverInterface> SatEngine::createSolver(const SolverSetup& setup) {
	std::shared_ptr<PortfolioSolverInterface> solver;
	switch (setup.solverType) {
	case 'l':
	case 'L':
		// Lingeling
		LOGGER(_logger, V4_VVER, "S%i : Lingeling-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new Lingeling(setup));
		break;
	case 'c':
	case 'C':
		// Cadical
		LOGGER(_logger, V4_VVER, "S%i : Cadical-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new Cadical(setup));
		break;
	case 'k':
	//case 'K': // no support for incremental mode as of now
		// Kissat
		LOGGER(_logger, V4_VVER, "S%i : Kissat-%i\n", setup.globalId, setup.diversificationIndex);
		solver.reset(new Kissat(setup));
		break;
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
	default:
		// Invalid solver
		LOGGER(_logger, V0_CRIT, "[ERROR] Invalid solver \"%c\" assigned\n", setup.solverType);
		_logger.flush();
		abort();
		break;
	}
	return solver;
}

void SatEngine::appendRevision(int revision, size_t fSize, const int* fLits, size_t aSize, const int* aLits, bool lastRevisionForNow) {
	
	LOGGER(_logger, V4_VVER, "Import rev. %i: %i lits, %i assumptions\n", revision, fSize, aSize);
	assert(_revision+1 == revision);
	_revision_data.push_back(RevisionData{fSize, fLits, aSize, aLits});
	_sharing_manager->setImportedRevision(revision);
	
	for (size_t i = 0; i < _num_solvers; i++) {
		if (revision == 0) {
			// Initialize solver thread
			_solver_threads.emplace_back(new SolverThread(
				_params, _config, _solver_interfaces[i], fSize, fLits, aSize, aLits, i
			));
		} else {
			if (_solver_interfaces[i]->getSolverSetup().doIncrementalSolving) {
				// True incremental SAT solving
				_solver_threads[i]->appendRevision(revision, fSize, fLits, aSize, aLits);
			} else {
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
				_sharing_manager->stopClauseImport(i);
				_solver_threads[i]->setTerminate();
				_obsolete_solver_threads.push_back(std::move(_solver_threads[i]));
				// Setup new solver and new solver thread
				SolverSetup s = _solver_interfaces[i]->getSolverSetup();
				s.solverRevision++;
				_solver_interfaces[i] = createSolver(s);
				_solver_threads[i] = std::shared_ptr<SolverThread>(new SolverThread(
					_params, _config, _solver_interfaces[i], 
					_revision_data[0].fSize, _revision_data[0].fLits, 
					_revision_data[0].aSize, _revision_data[0].aLits, 
					i
				));
				// Load entire formula 
				for (int importedRevision = 1; importedRevision <= revision; importedRevision++) {
					auto data = _revision_data[importedRevision];
					_solver_threads[i]->appendRevision(importedRevision, 
						data.fSize, data.fLits, data.aSize, data.aLits
					);
				}
				_sharing_manager->continueClauseImport(i);
				if (_solvers_started) _solver_threads[i]->start();
			}
		}
	}
	_revision = revision;
}

void SatEngine::solve() {
	assert(_revision >= 0);
	_result.result = UNKNOWN;
	if (!_solvers_started) {
		// Need to start threads
		LOGGER(_logger, V4_VVER, "starting threads\n");
		for (auto& thread : _solver_threads) thread->start();
		_solvers_started = true;
	}
	_state = ACTIVE;
}

bool SatEngine::isFullyInitialized() {
	if (_state == INITIALIZING) return false;
	for (size_t i = 0; i < _solver_threads.size(); i++) {
		if (!_solver_threads[i]->isInitialized()) return false;
	}
	return true;
}

int SatEngine::solveLoop() {
	if (isCleanedUp()) return -1;
	if (_block_result) return -1;

	// perform GC in export filter whenever necessary
	if (_sharing_manager) _sharing_manager->collectGarbageInFilter();

    // Solving done?
	bool done = false;
	for (size_t i = 0; i < _solver_threads.size(); i++) {
		if (_solver_threads[i]->hasFoundResult(_revision)) {

			if (_params.deterministicSolving() && _solver_interfaces[i]->getGlobalId() != _winning_solver_id)
				continue; // not the successful solver we're looking for

			auto& result = _solver_threads[i]->getSatResult();
			if (result.result > 0 && result.revision == _revision) {
				done = true;
				_result = std::move(result);
				_result.winningInstanceId = _solver_interfaces[i]->getGlobalId();
				_result.globalStartOfSuccessEpoch = _sharing_manager->getGlobalStartOfSuccessEpoch();
				break;
			}
		}
	}

	if (done) {
		LOGGER(_logger, V5_DEBG, "Returning result\n");
		return _result.result;
	}
    return -1; // no result yet
}

void SatEngine::setAllocatedSharingBufferSize(int allocatedSize) {
	_sharing_manager->setAllocatedSharingBufferSize(allocatedSize);
}

bool SatEngine::isReadyToPrepareSharing() const {
	// If certified UNSAT is enabled, no sharing operation can be ongoing
	// (otherwise, this op must be finished first, for clause ID consistency)
	return !ClauseMetadata::enabled() || !_sharing_manager->isSharingOperationOngoing();
}

void SatEngine::setClauseBufferRevision(int revision) {
	if (isCleanedUp()) return;
	_sharing_manager->setImportedRevision(revision);
}

int SatEngine::prepareSharing(int* begin, int maxSize, int& successfulSolverId, int& numLits) {
	if (isCleanedUp()) return sizeof(size_t) / sizeof(int); // checksum, nothing else
	LOGGER(_logger, V5_DEBG, "collecting clauses on this node\n");
	int size = _sharing_manager->prepareSharing(begin, maxSize, successfulSolverId, numLits);
	return size;
}

int SatEngine::filterSharing(int* begin, int size, int* filterOut) {
	if (isCleanedUp()) return 0;
	return _sharing_manager->filterSharing(begin, size, filterOut);
}

void SatEngine::addSharingEpoch(int epoch) {
	if (isCleanedUp()) return;
	_sharing_manager->addSharingEpoch(epoch);
}

void SatEngine::digestSharingWithFilter(int* begin, int size, const int* filter) {
	if (isCleanedUp()) return;
	_sharing_manager->digestSharingWithFilter(begin, size, filter);
}

void SatEngine::digestSharingWithoutFilter(int* begin, int size) {
	if (isCleanedUp()) return;
	_sharing_manager->digestSharingWithoutFilter(begin, size);
}

void SatEngine::returnClauses(int* begin, int size) {
	if (isCleanedUp()) return;
	_sharing_manager->returnClauses(begin, size);
}

void SatEngine::digestHistoricClauses(int epochBegin, int epochEnd, int* begin, int size) {
	if (isCleanedUp()) return;
	_sharing_manager->digestHistoricClauses(epochBegin, epochEnd, begin, size);
}

void SatEngine::syncDeterministicSolvingAndCheckForLocalWinner() {
	if (_block_result) {
		_block_result = !_sharing_manager->syncDeterministicSolvingAndCheckForWinningSolver();
	}
}

void SatEngine::reduceActiveThreadCount() {
	if (_num_active_solvers <= 1) return;

	// Reduce thread count by 10% (but at least one thread)
	size_t nbThreadsToTerminate = std::max(1UL, (size_t)std::round(0.1*_num_active_solvers));

	for (int termIdx = 0; termIdx < nbThreadsToTerminate; termIdx++) {
		size_t i = _num_active_solvers-1;
		LOGGER(_logger, V3_VERB, "Terminating %lu-th solver to reduce thread count\n", i);
		_sharing_manager->stopClauseImport(i);
		_solver_threads[i]->setTerminate(true);
		_num_active_solvers--;
	}
}

void SatEngine::dumpStats(bool final) {
	if (isCleanedUp() || !isFullyInitialized()) return;

	int verb = final ? V2_INFO : V4_VVER;

	// Solver statistics
	SolverStatistics solveStats;
	for (size_t i = 0; i < _num_solvers; i++) {
		SolverStatistics st = _solver_interfaces[i]->getSolverStats();
		int globalId = _solver_interfaces[i]->getGlobalId();
		_logger.log(verb, "%sS%d %s\n",
				final ? "END " : "", globalId, st.getReport().c_str());
		_logger.log(verb, "%sS%d clenhist prod %s\n",
				final ? "END " : "", globalId, st.histProduced->getReport().c_str());
		_logger.log(verb, "%sS%d clenhist digd %s\n",
				final ? "END " : "", globalId, st.histDigested->getReport().c_str());
		solveStats.aggregate(st);
	}
	_logger.log(verb, "%s%s\n", final ? "END " : "", solveStats.getReport().c_str());

	// Sharing statistics
	SharingStatistics shareStats;
	if (_sharing_manager != NULL) shareStats = _sharing_manager->getStatistics();
	_logger.log(verb, "%s%s\n", final ? "END " : "", shareStats.getReport().c_str());

	if (final) {
		// Histogram over clause lengths (do not print trailing zeroes)
		_logger.log(verb, "clenhist prod %s\n", shareStats.histProduced->getReport().c_str());
		_logger.log(verb, "clenhist flfl %s\n", shareStats.histFailedFilter->getReport().c_str());
		_logger.log(verb, "clenhist admt %s\n", shareStats.histAdmittedToDb->getReport().c_str());
		_logger.log(verb, "clenhist drpd %s\n", shareStats.histDroppedBeforeDb->getReport().c_str());
		_logger.log(verb, "clenhist dltd %s\n", shareStats.histDeletedInSlots->getReport().c_str());
		_logger.log(verb, "clenhist retd %s\n", shareStats.histReturnedToDb->getReport().c_str());

		// Flush logs
		for (auto& solver : _solver_interfaces) solver->getLogger().flush();
		_logger.flush();
	}
}

void SatEngine::setWinningSolverId(int globalId) {
	_sharing_manager->setWinningSolverId(globalId);
	_winning_solver_id = globalId;
}

void SatEngine::setPaused() {
	_state = SUSPENDED;
	for (auto& solver : _solver_threads) solver->setSuspend(true);
}

void SatEngine::unsetPaused() {
	_state = ACTIVE;
	for (auto& solver : _solver_threads) solver->setSuspend(false);
}

void SatEngine::terminateSolvers() {
	for (auto& solver : _solver_threads) {
		solver->setTerminate();
		solver->setSuspend(false);
	}
	dumpStats(/*final=*/true);
}

SatEngine::LastAdmittedStats SatEngine::getLastAdmittedClauseShare() {
	return LastAdmittedStats {
		_sharing_manager->getLastNumAdmittedClausesToImport(), 
		_sharing_manager->getLastNumClausesToImport(),
		_sharing_manager->getLastNumAdmittedLitsToImport()
	};
}

void SatEngine::writeClauseEpochs() {
	std::string filename = _params.logDirectory() + "/proof" 
		+ _config.getJobStr() + "/clauseepochs." + std::to_string(_config.apprank);
	_sharing_manager->writeClauseEpochs(/*_solver_interfaces[0]->getSolverSetup().proofDir, 
		_solver_interfaces[0]->getGlobalId(), */filename);
}

void SatEngine::cleanUp() {
	double time = Timer::elapsedSeconds();

	LOGGER(_logger, V5_DEBG, "[engine-cleanup] enter\n");

	// Terminate any remaining running threads
	terminateSolvers();
	
	// join and delete threads
	for (auto& thread : _solver_threads) thread->tryJoin();
	for (auto& thread : _obsolete_solver_threads) thread->tryJoin();
	_solver_threads.clear();
	_obsolete_solver_threads.clear();

	LOGGER(_logger, V5_DEBG, "[engine-cleanup] joined threads\n");

	// delete solvers
	_solver_interfaces.clear();
	LOGGER(_logger, V5_DEBG, "[engine-cleanup] cleared solvers\n");

	time = Timer::elapsedSeconds() - time;
	LOGGER(_logger, V4_VVER, "[engine-cleanup] done, took %.3f s\n", time);
	_logger.flush();

	if (ClauseMetadata::enabled()) writeClauseEpochs();

	_cleaned_up = true;
}

SatEngine::~SatEngine() {
	if (!_cleaned_up) cleanUp();
}

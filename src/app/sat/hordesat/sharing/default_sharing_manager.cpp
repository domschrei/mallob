/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include <signal.h>

#include "util/assert.hpp"

#include "default_sharing_manager.hpp"
#include "util/sys/timer.hpp"
#include "util/shuffle.hpp"

DefaultSharingManager::DefaultSharingManager(
		std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver, int jobIndex)
	: _solvers(solvers), _process_filter(/*maxClauseLen=*/params.strictClauseLengthLimit()), 
	_max_deferred_lits_per_solver(maxDeferredLitsPerSolver), 
	_params(params), _logger(logger), _job_index(jobIndex),
	_cdb([&]() {
		AdaptiveClauseDatabase::Setup setup;
		setup.maxClauseLength = _params.strictClauseLengthLimit();
		setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
		setup.numLiterals = _params.clauseBufferBaseSize()*_params.numChunksForExport();
		setup.numProducers = _solvers.size()+1;
		setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
		return setup;
	}()), _hist_produced(params.strictClauseLengthLimit()), 
	_hist_failed_filter(params.strictClauseLengthLimit()),
	_hist_admitted_to_db(params.strictClauseLengthLimit()), 
	_hist_dropped_before_db(params.strictClauseLengthLimit()),
	_hist_returned_to_db(params.strictClauseLengthLimit()) {

	_stats.histProduced = &_hist_produced;
	_stats.histFailedFilter = &_hist_failed_filter;
	_stats.histAdmittedToDb = &_hist_admitted_to_db;
	_stats.histDroppedBeforeDb = &_hist_dropped_before_db;
	_stats.histDeletedInSlots = &_cdb.getDeletedClausesHistogram();
	_stats.histReturnedToDb = &_hist_returned_to_db;

	auto callback = getCallback();
	
    for (size_t i = 0; i < _solvers.size(); i++) {
		_solver_filters.emplace_back(/*maxClauseLen=*/params.strictClauseLengthLimit());
		_solvers[i]->setExtLearnedClauseCallback(callback);
		_solver_revisions.push_back(_solvers[i]->getSolverSetup().solverRevision);
		_solver_stats.push_back(&_solvers[i]->getSolverStatsRef());
	}
	_last_buffer_clear = Timer::elapsedSeconds();
}

int DefaultSharingManager::prepareSharing(int* begin, int totalLiteralLimit) {

    //log(V5_DEBG, "Sharing clauses among %i nodes\n", size);
    static int prodInc = 1;
	static int lastInc = 0;

	int numExportedClauses = 0;
	auto buffer = _cdb.exportBuffer(totalLiteralLimit, numExportedClauses);
	//assert(buffer.size() <= maxSize);
	memcpy(begin, buffer.data(), buffer.size()*sizeof(int));

	LOGGER(_logger, V5_DEBG, "prepared %i clauses, size %i\n", numExportedClauses, buffer.size());
	_stats.exportedClauses += numExportedClauses;
	return buffer.size();
}

void DefaultSharingManager::digestSharing(std::vector<int>& result) {

	digestSharing(result.data(), result.size());
}

void DefaultSharingManager::digestSharing(int* begin, int buflen) {

	int verb = _job_index == 0 ? V3_VERB : V5_DEBG;
	float cfci = _params.clauseFilterClearInterval();
	float time = Timer::elapsedSeconds();
	ClauseHistogram hist(_params.strictClauseLengthLimit());

	digestDeferredFutureClauses();

	// Get all clauses
	auto reader = _cdb.getBufferReader(begin, buflen);

	// Convert clauses to plain format
	std::vector<Mallob::Clause> clauses;
	std::vector<uint32_t> producersPerClause;
	{
		auto clause = reader.getNextIncomingClause();
		size_t producerPos = 0;

		while (clause.begin != nullptr) {
			hist.increment(clause.size);
			for (size_t i = 0; i < clause.size; i++) assert(clause.begin[i] != 0);
			clauses.push_back(clause);
			producersPerClause.push_back(_cdb.getProducers(clause));
			clause = reader.getNextIncomingClause();
		}
	}

	// Any solvers not ready for import yet?
	bool deferringFutureClauses = false;
	for (int sid = 0; sid < _solvers.size(); sid++) {	
		
		auto& solver = _solvers[sid];

		// Defer clause "from the future"
		if (solver->getCurrentRevision() < _current_revision) {
			
			// Allocate a new entry in the deferred list with separate buffer
			// and clause objects (because the original buffer will go out of scope)
			if (!deferringFutureClauses) {
				deferringFutureClauses = true;
				_future_clauses.emplace_back();
				
				auto& d = _future_clauses.back();
				d.buffer = std::vector<int>(begin, begin+buflen);
				d.revision = _current_revision;
				d.involvedSolvers.resize(_solvers.size(), false);
				d.producersPerClause = producersPerClause;

				auto dReader = _cdb.getBufferReader(d.buffer.data(), buflen);
				auto clause = dReader.getNextIncomingClause();
				while (clause.begin != nullptr) {
					d.clauses.push_back(clause);
					clause = dReader.getNextIncomingClause();	
				}
				assert(d.clauses.size() == d.producersPerClause.size());
			}

			_future_clauses.back().involvedSolvers[sid] = true;
			continue;
		}

		// Import each clause passing the solver's filter
		importClausesToSolver(sid, clauses, producersPerClause);
	}
	
	// Clear all filters if necessary
	if (!_params.distributedDuplicateDetection()) {
		if (cfci == 0 || (cfci > 0 && Timer::elapsedSeconds() - _last_buffer_clear > cfci)) {
			_logger.log(verb, "clear filters\n");
			_process_filter.clear();
			for (auto& filter : _solver_filters) filter.setClear();
			_last_buffer_clear = Timer::elapsedSeconds();
		}
	}

	// Process-wide stats
	time = Timer::elapsedSeconds() - time;
	_logger.log(verb, "sharing time:%.4f %s\n", time, hist.getReport().c_str());
}

void DefaultSharingManager::digestDeferredFutureClauses() {

	for (auto it = _future_clauses.begin(); it != _future_clauses.end(); ++it) {
		auto& d = *it;
		bool solversRemaining = false;
		bool progress = false;
		for (size_t sid = 0; sid < _solvers.size(); sid++) {
			if (!d.involvedSolvers[sid]) continue;
			if (d.revision > _solvers[sid]->getCurrentRevision()) {
				// Not ready yet
				solversRemaining = true;
				continue;
			}
			// Ready to import
			importClausesToSolver(sid, d.clauses, d.producersPerClause);
			progress = true;
		}
		if (!solversRemaining) {
			// Erase this entry
			it = _future_clauses.erase(it);
			it--;
		}
		if (!progress) break;
	}
}

void DefaultSharingManager::importClausesToSolver(int solverId, const std::vector<Clause>& clauses, const std::vector<uint32_t>& producersPerClause) {

	assert(clauses.size() == producersPerClause.size());

	uint32_t producerFlag = 1 << solverId;
	assert(producerFlag >= 1 && producerFlag < 256);

	for (size_t cIdx = 0; cIdx < clauses.size(); cIdx++) {
		auto& c = clauses[cIdx];
		uint16_t producers = producersPerClause[cIdx];
		if ((producers & producerFlag) != 0) {
			// Clause was produced by this solver!
			log(V2_INFO, "%i : FILTERED (%u & %u != 0) %s\n", solverId, producers, producerFlag, c.toStr().c_str());

			// TODO This mirroring filter sometimes does not work if different solvers export a clause
			// with different LBD values - the producer of the lower LBD clause will be acknowledged
			// while the producer of the higher LBD clause may be neglected (because the buffer export did not reach that clause), 
			// so it may be mirrored the clause.
			// This MAY be considered a feature - the mirrored clause is of a better LBD value than the exported one.
			// But this is not consistent because clauses are not generally re-shared if a better LBD is found.
			// Possible solution: change map [size,lbd,data]->producers to [size,data]->[lbd,producers].

			// Also, the mirroring filter is based on a snapshot during buffer export, so the clause may have been
			// exported by additional solvers in the meantime.

			// Could think about integrating the mirroring filter into the clause slots themselves: Do not directly
			// delete clauses at export, but keep them until the next import. At import, clauses can be checked
			// by whom they have already been exported (as of NOW) and given to the remaining solvers. If SOME producer
			// was found, delete the clause, otherwise keep it. => You only remove clauses from the database if they were
			// shared successfully (or to make room for better clauses). This renders the "returnClauses" mechanic obsolete as well.

			continue;
		}
		// Old solver filter
		if (!_params.distributedDuplicateDetection() && !_solver_filters[solverId].registerClause(c.begin, c.size)) 
			continue;

		_solvers[solverId]->addLearnedClause(c);
		log(V2_INFO, "%i : IMPORTED (%u & %u == 0) %s\n", solverId, producers, producerFlag, c.toStr().c_str());
	}
}

void DefaultSharingManager::processClause(int solverId, int solverRevision, const Clause& clause, int condVarOrZero) {
	
	auto& solverStats = _solver_stats[solverId];
	if (solverStats) {
		solverStats->producedClauses++;
		solverStats->histProduced->increment(clause.size);
	}
	
	if (_solver_revisions[solverId] != solverRevision) return;

	if (_params.crashMonkeyProbability() > 0) {
		if (Random::rand() < _params.crashMonkeyProbability()) {
			// Crash!
			LOGGER(_logger, V3_VERB, "Simulating a crash!\n");
			raise(SIGSEGV); // causes crash
		}
	}

	auto clauseBegin = clause.begin;
	auto clauseSize = clause.size;

	// If necessary, apply a transformation to the clause:
	// Add the supplied conditional variable in negated form to the clause.
	// This effectively renders the found conflict relative to the assumptions
	// which were added not as assumptions but as permanent unit clauses.
	std::vector<int>* tldClauseVec = nullptr;
	if (condVarOrZero != 0) {
		tldClauseVec = new std::vector<int>(clause.size+1);
		for (int i = 0; i < clause.size; i++) tldClauseVec->at(i) = clause.begin[i];
		tldClauseVec->at(clause.size) = -condVarOrZero;
		clauseBegin = tldClauseVec->data();
		clauseSize++;
	}

	/*
	if (clauseSize == 1 && clause.lbd != 1) {
		_logger.log(V1_WARN, "Observed unit LBD of %i\n", clause.lbd);
	}
	if (clauseSize > 1) {
		_observed_nonunit_lbd_of_zero |= clause.lbd == 0;
		_observed_nonunit_lbd_of_one |= clause.lbd == 1;
		_observed_nonunit_lbd_of_two |= clause.lbd == 2;
		_observed_nonunit_lbd_of_length_minus_one |= clause.lbd == clause.size-1;
		_observed_nonunit_lbd_of_length |= clause.lbd == clause.size;
	}
	*/

	if (clauseSize == 1) assert(clause.lbd == 1);
	else {
		assert(clause.lbd >= 1 || LOG_RETURN_FALSE("[ERROR] len=%i lbd=%i!\n", clause.size, clause.lbd));
		assert(clause.lbd <= clause.size);
	}

	// Add clause length to statistics
	_hist_produced.increment(clauseSize);

	// Sort and write clause into database if possible
	std::sort(clauseBegin, clauseBegin+clauseSize);
	Clause tldClause(clauseBegin, clauseSize, clauseSize == 1 ? 1 : std::max(2, clause.lbd));
	auto result = _cdb.addClause(solverId, tldClause);

	if (result == SUCCESS) {
		_hist_admitted_to_db.increment(clauseSize);
		if (solverStats) solverStats->producedClausesAdmitted++;
	} else if (result == NO_BUDGET) {
		// completely dropping the clause
		_hist_dropped_before_db.increment(clauseSize);
		_stats.clausesDroppedAtExport++;
		if (solverStats) solverStats->producedClausesDropped++;
	} else {
		// duplicate
		_hist_failed_filter.increment(clauseSize);
		_stats.clausesProcessFilteredAtExport++;
		if (solverStats) solverStats->producedClausesSolverFiltered++;
	}
	if (result != NO_BUDGET) log(V2_INFO, "%i : EXPORTED %s\n", solverId, tldClause.toStr().c_str());

	if (tldClauseVec) delete tldClauseVec;
}

void DefaultSharingManager::returnClauses(int* begin, int buflen) {

	auto reader = _cdb.getBufferReader(begin, buflen);
	auto c = reader.getNextIncomingClause();
	while (c.begin != nullptr) {
		if (_params.distributedDuplicateDetection() || _process_filter.registerClause(c.begin, c.size)) {
			_cdb.addClause(_solvers.size(), c);
			_hist_returned_to_db.increment(c.size);
		}
		c = reader.getNextIncomingClause();
	}
}

SharingStatistics DefaultSharingManager::getStatistics() {
	/*
	_logger.log(V2_INFO, "Observed non-unit LBDs: 0:%i 1:%i 2:%i len-1:%i len:%i\n", 
		_observed_nonunit_lbd_of_zero, 
		_observed_nonunit_lbd_of_one, 
		_observed_nonunit_lbd_of_two, 
		_observed_nonunit_lbd_of_length_minus_one, 
		_observed_nonunit_lbd_of_length);
	*/
	return _stats;
}

void DefaultSharingManager::stopClauseImport(int solverId) {
	assert(solverId >= 0 && solverId < _solvers.size());
	_solver_revisions[solverId] = -1;
	_solver_stats[solverId] = nullptr;
}

void DefaultSharingManager::continueClauseImport(int solverId) {
	assert(solverId >= 0 && solverId < _solvers.size());
	_solver_revisions[solverId] = _solvers[solverId]->getSolverSetup().solverRevision;
	_solvers[solverId]->setExtLearnedClauseCallback(getCallback());
	_solver_stats[solverId] = &_solvers[solverId]->getSolverStatsRef();
}

DefaultSharingManager::~DefaultSharingManager() {}

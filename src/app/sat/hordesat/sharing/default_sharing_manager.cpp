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
	_cdb(
		/*maxClauseSize=*/_params.strictClauseLengthLimit(),
		/*maxLbdPartitionedSize=*/_params.maxLbdPartitioningSize(),
		/*baseBufferSize=*/_params.clauseBufferBaseSize(),
		/*numChunks=*/_params.numChunksForExport(),
		/*numProducers=*/_solvers.size()+1
	), _hist_produced(params.strictClauseLengthLimit()), 
	_hist_admitted_to_db(params.strictClauseLengthLimit()), 
	_hist_dropped_before_db(params.strictClauseLengthLimit()),
	_hist_returned_to_db(params.strictClauseLengthLimit()) {

	_stats.histProduced = &_hist_produced;
	_stats.histAdmittedToDb = &_hist_admitted_to_db;
	_stats.histDroppedBeforeDb = &_hist_dropped_before_db;
	_stats.histDeletedInSlots = &_cdb.getDeletedClausesHistogram();
	_stats.histReturnedToDb = &_hist_returned_to_db;

	auto callback = getCallback();
	
    for (size_t i = 0; i < _solvers.size(); i++) {
		_solver_filters.emplace_back(/*maxClauseLen=*/params.strictClauseLengthLimit());
		_solvers[i]->setExtLearnedClauseCallback(callback);
		_solver_revisions.push_back(_solvers[i]->getSolverSetup().solverRevision);
		_deferred_admitted_clauses.emplace_back();
		_solver_stats.push_back(&_solvers[i]->getSolverStatsRef());
	}
	_last_buffer_clear = Timer::elapsedSeconds();
}

int DefaultSharingManager::prepareSharing(int* begin, int maxSize) {

    //log(V5_DEBG, "Sharing clauses among %i nodes\n", size);
    static int prodInc = 1;
	static int lastInc = 0;

	int numExportedClauses = 0;
	auto buffer = _cdb.exportBuffer(maxSize, numExportedClauses);
	assert(buffer.size() <= maxSize);
	memcpy(begin, buffer.data(), buffer.size()*sizeof(int));

	LOGGER(_logger, V5_DEBG, "prepared %i clauses, size %i\n", numExportedClauses, buffer.size());
	_stats.exportedClauses += numExportedClauses;
	float usedRatio = ((float)buffer.size())/maxSize;
	LOGGER(_logger, V5_DEBG, "buffer fillratio=%.3f\n", usedRatio);
	
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
	{
		auto clause = reader.getNextIncomingClause();
		while (clause.begin != nullptr) {
			hist.increment(clause.size);
			for (size_t i = 0; i < clause.size; i++) assert(clause.begin[i] != 0);
			clauses.push_back(clause);
			clause = reader.getNextIncomingClause();
		}
	}

	// Any solvers not ready for import yet?
	bool deferringFutureClauses = false;
	for (int sid = 0; sid < _solvers.size(); sid++) {	
		
		// Defer clause "from the future"
		if (_solvers[sid]->getCurrentRevision() < _current_revision) {
			
			// Allocate a new entry in the deferred list with separate buffer
			// and clause objects (because the original buffer will go out of scope)
			if (!deferringFutureClauses) {
				deferringFutureClauses = true;
				_future_clauses.emplace_back();
				
				auto& d = _future_clauses.back();
				d.buffer = std::vector<int>(begin, begin+buflen);
				d.revision = _current_revision;
				d.involvedSolvers.resize(_solvers.size(), false);

				auto dReader = _cdb.getBufferReader(d.buffer.data(), buflen);
				auto clause = dReader.getNextIncomingClause();
				while (clause.begin != nullptr) {
					d.clauses.push_back(clause);
					clause = dReader.getNextIncomingClause();	
				}
			}

			_future_clauses.back().involvedSolvers[sid] = true;
			continue;
		}

		// Import each clause passing the solver's filter
		_solvers[sid]->addLearnedClauses(clauses, [&](const Clause& c) {
			return _solver_filters[sid].registerClause(c.begin, c.size);
		});
	}
	
	// Clear all filters if necessary
	if (cfci == 0 || (cfci > 0 && Timer::elapsedSeconds() - _last_buffer_clear > cfci)) {
		_logger.log(verb, "clear filters\n");
		_process_filter.clear();
		for (auto& filter : _solver_filters) filter.setClear();
		_last_buffer_clear = Timer::elapsedSeconds();
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
			_solvers[sid]->addLearnedClauses(d.clauses, [&](const Clause& c) {
				return _solver_filters[sid].registerClause(c.begin, c.size);
			});
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

	// Add clause length to statistics
	_hist_produced.increment(clauseSize);

	// Register clause in both process filter and solver filter
	bool success = true;
	if (!_solver_filters[solverId].registerClause(clauseBegin, clauseSize)) {
		// Failed solver filter
		_stats.clausesSolverFilteredAtExport++;
		if (solverStats) solverStats->producedClausesSolverFiltered++;
		success = false;
	} else if (!_process_filter.registerClause(clauseBegin, clauseSize)) {
		// Failed process filter
		_stats.clausesProcessFilteredAtExport++;
		if (solverStats) solverStats->producedClausesProcessFiltered++;
		success = false;
	}
	if (!success) {
		if (tldClauseVec) delete tldClauseVec;
		return;
	}
	
	// Sort and write clause into database if possible
	success = false;
	std::sort(clauseBegin, clauseBegin+clauseSize);
	Clause tldClause(clauseBegin, clauseSize, clauseSize == 1 ? 1 : (clauseSize == 2 ? 2 : clause.lbd));
	auto result = _cdb.addClause(solverId, tldClause);
	if (result == AdaptiveClauseDatabase::TRY_LATER) {
		// copy and defer clause to insert at a later time
		_deferred_admitted_clauses[solverId].push_back(tldClause.copy());
		_stats.clausesDeferredAtExport++;
		if (solverStats) solverStats->producedClausesDeferred++;
	} else if (result == AdaptiveClauseDatabase::DROP) {
		// completely dropping the clause
		_stats.clausesDroppedAtExport++;
		if (solverStats) solverStats->producedClausesDropped++;
		_hist_dropped_before_db.increment(clauseSize);
	} else {
		// success
		success = true;
		_hist_admitted_to_db.increment(clauseSize);
		if (solverStats) solverStats->producedClausesAdmitted++;
	}
	if (tldClauseVec) delete tldClauseVec;

	if (!success) return;
	
	// If just successfully inserted a clause, also try to insert earlier deferred clauses
	tryReinsertDeferredClauses(solverId, _deferred_admitted_clauses[solverId], solverStats);
}

void DefaultSharingManager::returnClauses(int* begin, int buflen) {

	tryReinsertDeferredClauses(_solvers.size(), _deferred_returned_clauses, &_returned_clauses_stats);

	// Convert to clause vector
	auto reader = _cdb.getBufferReader(begin, buflen);
	std::vector<Clause> clauseVec;
	auto c = reader.getNextIncomingClause();
	while (c.begin != nullptr) {
		_hist_returned_to_db.increment(c.size);
		clauseVec.push_back(c);
		c = reader.getNextIncomingClause();
	}

	// Add clauses to clause database
	_cdb.bulkAddClauses(_solvers.size(), clauseVec, _deferred_returned_clauses, _returned_clauses_stats, [&](const Clause& c) {
		return _process_filter.registerClause(c.begin, c.size);
	});
}

void DefaultSharingManager::tryReinsertDeferredClauses(int solverId, std::list<Clause>& clauseList, SolvingStatistics* stats) {
	for (auto it = clauseList.begin(); it != clauseList.end(); ++it) {
		Clause& c = *it;
		auto result = _cdb.addClause(solverId, c);
		if (result == AdaptiveClauseDatabase::TRY_LATER) break;
		
		if (result == AdaptiveClauseDatabase::DROP) {
			// completely dropping the clause
			_stats.clausesDroppedAtExport++;
			if (stats) stats->producedClausesDropped++;
		}
		if (result == AdaptiveClauseDatabase::SUCCESS) {
			_hist_admitted_to_db.increment(c.size);
			if (stats) stats->producedClausesAdmitted++;
		}
		// clause admitted or discarded: free malloc'd clause
		free(c.begin);
		it = clauseList.erase(it);
		it--;
	}
}

SharingStatistics DefaultSharingManager::getStatistics() {
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

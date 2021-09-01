/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include "util/assert.hpp"

#include "default_sharing_manager.hpp"
#include "util/sys/timer.hpp"
#include "util/shuffle.hpp"

DefaultSharingManager::DefaultSharingManager(
		std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver, int jobIndex)
	: _solvers(solvers), _deferred_clauses(_solvers.size()), 
	_max_deferred_lits_per_solver(maxDeferredLitsPerSolver), 
	_params(params), _logger(logger), _job_index(jobIndex),
	_cdb(
		/*maxClauseSize=*/_params.hardMaxClauseLength(),
		/*maxLbdPartitionedSize=*/_params.maxLbdPartitioningSize(),
		/*baseBufferSize=*/_params.clauseBufferBaseSize(),
		/*numProducers=*/_solvers.size()
	) {

	memset(_seen_clause_len_histogram, 0, CLAUSE_LEN_HIST_LENGTH*sizeof(unsigned long));
	_stats.seenClauseLenHistogram = _seen_clause_len_histogram;

	auto callback = getCallback();
	
    for (size_t i = 0; i < _solvers.size(); i++) {
		_solver_filters.emplace_back(/*maxClauseLen=*/params.hardMaxClauseLength(), /*checkUnits=*/true);
		_solvers[i]->setExtLearnedClauseCallback(callback);
		_solver_revisions.push_back(_solvers[i]->getSolverSetup().solverRevision);
	}
	_last_buffer_clear = Timer::elapsedSeconds();
}

int DefaultSharingManager::prepareSharing(int* begin, int maxSize) {

    //log(V5_DEBG, "Sharing clauses among %i nodes\n", size);
    static int prodInc = 1;
	static int lastInc = 0;

	int numExportedClauses = 0;
	auto buffer = _cdb.exportBuffer(maxSize, numExportedClauses);
	memcpy(begin, buffer.data(), buffer.size()*sizeof(int));

	_logger.log(V5_DEBG, "prepared %i clauses, size %i\n", numExportedClauses, buffer.size());
	_stats.exportedClauses += numExportedClauses;
	float usedRatio = ((float)buffer.size())/maxSize;
	if (usedRatio < _params.increaseClauseProductionRatio()) {
		int increaser = lastInc++ % _solvers.size();
		_solvers[increaser]->increaseClauseProduction();
		_logger.log(V3_VERB, "prod. increase no. %d (sid %d)\n", prodInc++, increaser);
	}
	_logger.log(V5_DEBG, "buffer fillratio=%.3f\n", usedRatio);
	
	return buffer.size();
}

void DefaultSharingManager::digestSharing(std::vector<int>& result) {

	digestSharing(result.data(), result.size());
}

void DefaultSharingManager::digestSharing(int* begin, int buflen) {
	digestDeferredClauses();
    
	// Get all clauses
	auto reader = _cdb.getBufferReader(begin, buflen);
	
	size_t numClauses = 0;
	std::vector<int> lens;
	std::vector<int> added(_solvers.size(), 0);
	std::vector<int> deferred(_solvers.size(), 0);

	bool shuffleClauses = _params.shuffleSharedClauses();
	
	// For each incoming clause:
	auto clause = reader.getNextIncomingClause();
	while (clause.begin != nullptr) {
		
		numClauses++;

		// Shuffle clause (NOT the 1st position as this is the glue score)
		if (shuffleClauses) shuffle(clause.begin, clause.size);
		
		// Clause length stats
		while (clause.size-1 >= (int)lens.size()) lens.push_back(0);
		lens[clause.size-1]++;

		// Import clause into each solver if its filter allows it
		for (size_t sid = 0; sid < _solvers.size(); sid++) {
			
			// Defer clause "from the future"
			if (_solvers[sid]->getCurrentRevision() < _current_revision) {
				if (addDeferredClause(sid, clause)) deferred[sid]++;
				continue;
			}

			if (_solver_filters[sid].registerClause(clause.begin, clause.size)) {
				_solvers[sid]->addLearnedClause(clause);
				added[sid]++;
			}
		}

		clause = reader.getNextIncomingClause();
	}
	_stats.importedClauses += numClauses;
	
	if (numClauses == 0) return;

	// Process-wide stats
	std::string lensStr = "";
	for (int len : lens) lensStr += std::to_string(len) + " ";
	int verb = _job_index == 0 ? V3_VERB : V5_DEBG;
	_logger.log(verb, "sharing total=%d lens %s\n", numClauses, lensStr.c_str());
	// Per-solver stats
	for (size_t sid = 0; sid < _solvers.size(); sid++) {
		_logger.log(V3_VERB, "S%d imp=%d def=%d\n", _solvers[sid]->getGlobalId(), added[sid], deferred[sid]);
	}

	float cfhl = _params.clauseFilterHalfLife();
	// Clear all filters if necessary
	if ((int)cfhl == 0) {
		for (auto& filter : _solver_filters) filter.clear();
	}
	// Clear half of the clauses from the filter (probabilistically) if a clause filter half life is set
	if (cfhl > 0 && Timer::elapsedSeconds() - _last_buffer_clear > cfhl) {
		_logger.log(verb, "forget half of clauses in filters\n");
		for (size_t sid = 0; sid < _solver_filters.size(); sid++) {
			_solver_filters[sid].clearHalf();
		}
		_last_buffer_clear = Timer::elapsedSeconds();
	}
}

bool DefaultSharingManager::addDeferredClause(int solverId, const Clause& c) {
		
	// Create copied clause
	std::vector<int> clsVec(c.size+1);
	clsVec[0] = c.lbd;
	for (size_t i = 0; i < c.size; i++) clsVec[i+1] = c.begin[i];

	// Find list to append clause to
	auto& lists = _deferred_clauses[solverId];
	if (lists.empty() || lists.back().revision < _current_revision) {
		// No list for this revision yet: Append new list
		DeferredClauseList list;
		list.revision = _current_revision;
		lists.push_back(std::move(list));
	} else if (lists.back().numLits + c.size+1 > _max_deferred_lits_per_solver) {
		return false; // limit on deferred clauses' volume reached
	}

	// Append clause to correct list
	lists.back().clauses.push_back(std::move(clsVec));
	lists.back().numLits += c.size+1;
	return true;
}

void DefaultSharingManager::digestDeferredClauses() {

	for (size_t sid = 0; sid < _solvers.size(); sid++) {
		size_t numAdded = 0;	
		auto& lists = _deferred_clauses[sid];
		for (auto it = lists.begin(); it != lists.end(); it++) {
			DeferredClauseList& list = *it;
			if (list.revision > _solvers[sid]->getCurrentRevision()) {
				// Not ready yet: all subsequent lists are not ready as well
				break;
			}
			// Ready to import!
			for (auto& cls : list.clauses) {
				Clause c;
				c.lbd = cls[0];
				c.begin = cls.data()+1;
				c.size = cls.size()-1;
				if (_solver_filters[sid].registerClause(c.begin, c.size)) {
					if (numAdded == 0) {
						_logger.log(V5_DEBG, "S%i receives deferred cls\n", _solvers[sid]->getGlobalId());
					}
					_solvers[sid]->addLearnedClause(c);
					numAdded++;
				}
			}
			// Remove clause list
			it = lists.erase(it);
			it--;
		}
		if (numAdded > 0) _logger.log(V4_VVER, "S%i added %i deferred cls\n", _solvers[sid]->getGlobalId(), numAdded);
	}
}

void DefaultSharingManager::processClause(int solverId, int solverRevision, const Clause& clause, int condVarOrZero) {
	if (_solver_revisions[solverId] != solverRevision) return;

	auto clauseBegin = clause.begin;
	auto clauseSize = clause.size;

	// If necessary, apply a transformation to the clause:
	// Add the supplied conditional variable in negated form to the clause.
	// This effectively renders the found conflict relative to the assumptions
	// which were added not as assumptions but as permanent unit clauses.
	std::vector<int> tldClause;
	if (condVarOrZero != 0) {
		tldClause.insert(tldClause.end(), clause.begin, clause.begin+clause.size);
		tldClause.push_back(-condVarOrZero);
		clauseBegin = tldClause.data();
		clauseSize++;
	}

	// Add clause length to statistics
	_seen_clause_len_histogram[std::min(clauseSize, CLAUSE_LEN_HIST_LENGTH)-1]++;

	// Register clause in this solver's filter
	if (_solver_filters[solverId].registerClause(clauseBegin, clauseSize)) {
		// Success - sort and write clause into database if possible
		std::sort(clauseBegin, clauseBegin+clauseSize);
		Clause tldClause{clauseBegin, clauseSize, clauseSize == 1 ? 1 : (clauseSize == 2 ? 2 : clause.lbd)};
		if (!_cdb.addClause(solverId, tldClause)) _stats.clausesDroppedAtExport++;
	} else {
		// Clause was already registered before
		_stats.clausesFilteredAtExport++;
	}
}

SharingStatistics DefaultSharingManager::getStatistics() {
	return _stats;
}

void DefaultSharingManager::stopClauseImport(int solverId) {
	assert(solverId >= 0 && solverId < _solvers.size());
	_solver_revisions[solverId] = -1;
}

void DefaultSharingManager::continueClauseImport(int solverId) {
	assert(solverId >= 0 && solverId < _solvers.size());
	_solver_revisions[solverId] = _solvers[solverId]->getSolverSetup().solverRevision;
	_solvers[solverId]->setExtLearnedClauseCallback(getCallback());
}

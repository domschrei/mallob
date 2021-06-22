/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include <assert.h>

#include "default_sharing_manager.hpp"
#include "util/sys/timer.hpp"
#include "util/shuffle.hpp"

DefaultSharingManager::DefaultSharingManager(
		std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const Logger& logger)
	: _solvers(solvers), _params(params), _logger(logger), 
	_cdb(
		/*maxClauseSize=*/_params.getIntParam("hmcl"), 
		/*maxLbdPartitionedSize=*/_params.getIntParam("mlbdps"),
		/*baseBufferSize=*/_params.getIntParam("cbbs"), 
		/*numProducers=*/_solvers.size()
	) {

	memset(_seen_clause_len_histogram, 0, CLAUSE_LEN_HIST_LENGTH*sizeof(unsigned long));
	_stats.seenClauseLenHistogram = _seen_clause_len_histogram;

	auto callback = getCallback();
	
    for (size_t i = 0; i < _solvers.size(); i++) {
		_solver_filters.emplace_back(/*maxClauseLen=*/params.getIntParam("hmcl", 0), /*checkUnits=*/true);
		_solvers[i]->setExtLearnedClauseCallback(callback);
		_importing.push_back(true);
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
	if (usedRatio < _params.getFloatParam("icpr")) {
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
    
	// Get all clauses
	auto reader = _cdb.getBufferReader(begin, buflen);
	
	size_t numClauses = 0;
	std::vector<int> lens;
	std::vector<int> added(_solvers.size(), 0);

	bool shuffleClauses = _params.isNotNull("shufshcls");
	
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
	_logger.log(V3_VERB, "sharing total=%d lens %s\n", numClauses, lensStr.c_str());
	// Per-solver stats
	for (size_t sid = 0; sid < _solvers.size(); sid++) {
		_logger.log(V3_VERB, "S%d imp=%d\n", sid, numClauses-added[sid]);
	}

	// Clear half of the clauses from the filter (probabilistically) if a clause filter half life is set
	if (_params.getIntParam("cfhl", 0) > 0 && Timer::elapsedSeconds() - _last_buffer_clear > _params.getIntParam("cfhl", 0)) {
		_logger.log(V3_VERB, "forget half of clauses in filters\n");
		for (size_t sid = 0; sid < _solver_filters.size(); sid++) {
			_solver_filters[sid].clearHalf();
		}
		_last_buffer_clear = Timer::elapsedSeconds();
	}
}

void DefaultSharingManager::processClause(int solverId, const Clause& clause, int condVarOrZero) {
	if (!_importing[solverId]) return;

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
		// Success - write clause into database if possible
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
	_importing[solverId] = false;
}

void DefaultSharingManager::continueClauseImport(int solverId) {
	assert(solverId >= 0 && solverId < _solvers.size());
	_importing[solverId] = true;
	_solvers[solverId]->setExtLearnedClauseCallback(getCallback());
}

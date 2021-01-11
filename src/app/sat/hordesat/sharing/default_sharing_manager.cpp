/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include <assert.h>

#include "default_sharing_manager.hpp"
#include "util/sys/timer.hpp"

DefaultSharingManager::DefaultSharingManager(
		std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const Logger& logger)
	: _solvers(solvers), _params(params), _logger(logger), _cdb(logger) {

	memset(_seen_clause_len_histogram, 0, CLAUSE_LEN_HIST_LENGTH*sizeof(unsigned long));
	_stats.seenClauseLenHistogram = _seen_clause_len_histogram;

	auto callback = [this](std::vector<int>& cls, int solverId) {processClause(cls, solverId);};
	
    for (size_t i = 0; i < _solvers.size(); i++) {
		_solver_filters.emplace_back(/*maxClauseLen=*/params.getIntParam("hmcl", 0), /*checkUnits=*/true);
		_solvers[i]->setLearnedClauseCallback(callback);
	}
	_last_buffer_clear = Timer::elapsedSeconds();
}

int DefaultSharingManager::prepareSharing(int* begin, int maxSize) {

    //log(V5_DEBG, "Sharing clauses among %i nodes\n", size);
    static int prodInc = 1;
	static int lastInc = 0;

	int selectedCount;
	int used = _cdb.giveSelection(begin, maxSize, &selectedCount);
	_logger.log(V5_DEBG, "Prepared %i clauses, size %i\n", selectedCount, used);
	_stats.exportedClauses += selectedCount;
	float usedRatio = ((float)used)/maxSize;
	if (usedRatio < _params.getFloatParam("icpr")) {
		int increaser = lastInc++ % _solvers.size();
		_solvers[increaser]->increaseClauseProduction();
		_logger.log(V5_DEBG, "Production increase for %d. time, sid %d will increase.\n", prodInc++, increaser);
	}
	_logger.log(V5_DEBG, "buffer fillratio=%.3f\n", usedRatio);
	return used;
}

void DefaultSharingManager::digestSharing(const std::vector<int>& result) {

	digestSharing(result.data(), result.size());
}

void DefaultSharingManager::digestSharing(const int* begin, int buflen) {
    
	// Get all clauses
	_cdb.setIncomingBuffer(begin, buflen);
	
	size_t numClauses = 0;
	std::vector<int> lens;
	std::vector<int> added(_solvers.size(), 0);

	// For each incoming clause:
	int size;
	const int* clsbegin = _cdb.getNextIncomingClause(size);
	while (clsbegin != NULL) {
		
		numClauses++;
		int clauseLen = (size > 1 ? size-1 : size); // subtract "glue" int
		
		// Clause length stats
		while (clauseLen-1 >= (int)lens.size()) lens.push_back(0);
		lens[clauseLen-1]++;

		// Import clause into each solver if its filter allows it
		for (size_t sid = 0; sid < _solvers.size(); sid++) {
			if (_solver_filters[sid].registerClause(clsbegin, size)) {
				_solvers[sid]->addLearnedClause(clsbegin, size);
				added[sid]++;
			}
		}

		clsbegin = _cdb.getNextIncomingClause(size);
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

void DefaultSharingManager::processClause(std::vector<int>& cls, int solverId) {

	// Add clause length to statistics
	_seen_clause_len_histogram[cls.size() == 1 ? 1 : std::min(cls.size(), (size_t)CLAUSE_LEN_HIST_LENGTH)-1]++;

	// Register clause in this solver's filter
	if (_solver_filters[solverId].registerClause(cls)) {
		// Success - write clause into database if possible
		if (_cdb.addClause(cls) == NULL) _stats.clausesDroppedAtExport++;
	} else {
		// Clause was already registered before
		_stats.clausesFilteredAtExport++;
	}
}

SharingStatistics DefaultSharingManager::getStatistics() {
	return _stats;
}

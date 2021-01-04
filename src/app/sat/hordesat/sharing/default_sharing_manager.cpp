/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include <assert.h>

#include "default_sharing_manager.hpp"

DefaultSharingManager::DefaultSharingManager(
		std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const LoggingInterface& logger)
	: _solvers(solvers), _params(params), _logger(logger), _cdb(logger), 
		_node_filter(/*maxClauseLen=*/params.getIntParam("hmcl", 0), /*checkUnits=*/true) {

	memset(_seen_clause_len_histogram, 0, CLAUSE_LEN_HIST_LENGTH*sizeof(unsigned long));
	_stats.seenClauseLenHistogram = _seen_clause_len_histogram;

	auto callback = [this](std::vector<int>& cls, int solverId) {processClause(cls, solverId);};
	
    for (size_t i = 0; i < _solvers.size(); i++) {
		if (_solvers.size() > 1) {
			_solver_filters.emplace_back(/*maxClauseLen=*/params.getIntParam("hmcl", 0), /*checkUnits=*/true);
		}
		_solvers[i]->setLearnedClauseCallback(callback);
	}
	_last_buffer_clear = logger.getTime();
}

int DefaultSharingManager::prepareSharing(int* begin, int maxSize) {

    //log(3, "Sharing clauses among %i nodes\n", size);
    static int prodInc = 1;
	static int lastInc = 0;
	if (!_params.isNotNull("fd")) {
		_logger.log(2, "clear node clsfltr\n");
		_node_filter.clear();
	}
	int selectedCount;
	int used = _cdb.giveSelection(begin, maxSize, &selectedCount);
	_logger.log(3, "Prepared %i clauses, size %i\n", selectedCount, used);
	_stats.exportedClauses += selectedCount;
	float usedRatio = ((float)used)/maxSize;
	if (usedRatio < _params.getFloatParam("icpr")) {
		int increaser = lastInc++ % _solvers.size();
		_solvers[increaser]->increaseClauseProduction();
		_logger.log(3, "Production increase for %d. time, sid %d will increase.\n", prodInc++, increaser);
	}
	_logger.log(3, "Filled %.1f%% of buffer\n", 100*usedRatio);
	return used;
}

void DefaultSharingManager::digestSharing(const std::vector<int>& result) {

	digestSharing(result.data(), result.size());
}

void DefaultSharingManager::digestSharing(const int* begin, int buflen) {
    
	// Get all clauses
	_cdb.setIncomingBuffer(begin, buflen);
	
	int passedFilter = 0;
	int failedFilter = 0;
	std::vector<int> lens;
	std::vector<int> added(_solvers.size(), 0);

	// For each incoming clause:
	int size;
	const int* clsbegin = _cdb.getNextIncomingClause(size);
	while (clsbegin != NULL) {
		
		int clauseLen = (size > 1 ? size-1 : size); // subtract "glue" int
		while (clauseLen-1 >= (int)lens.size()) lens.push_back(0);
		lens[clauseLen-1]++;

		if (_solvers.size() > 1) {
			// Multiple solvers: Employing the node filter here would prevent 
			// local sharing, i.e., if solver 0 on node n learns a clause
			// then solvers 1..k on node n will never receive it.
			// So only use the per-solver filters.
			bool anyPassed = false;
			for (size_t sid = 0; sid < _solvers.size(); sid++) {
				if (_solver_filters[sid].registerClause(clsbegin, size)) {
					_solvers[sid]->addLearnedClause(clsbegin, size);
					added[sid]++;
					anyPassed = true;
				}
			}
			if (anyPassed) passedFilter++;
			else failedFilter++;
		} else {
			// Only one solver on this node: No per-solver filtering.
			// Use the node filter in this case to prevent redundant learning.
			if (_node_filter.registerClause(clsbegin, size)) {
				_solvers[0]->addLearnedClause(clsbegin, size);
				passedFilter++;
			} else {
				failedFilter++;
			}
		}

		clsbegin = _cdb.getNextIncomingClause(size);
	}
	int total = passedFilter + failedFilter;
	_stats.importedClauses += passedFilter;
	_stats.clausesFilteredAtImport += failedFilter;
	
	if (total == 0) return;

	// Process-wide stats
	std::string lensStr = "";
	for (int len : lens) lensStr += std::to_string(len) + " ";
	_logger.log(1, "disc %d total %d lens %s\n",
			failedFilter, total, lensStr.c_str());

	// Per-solver stats
	if (_solvers.size() > 1) {
		for (size_t sid = 0; sid < _solvers.size(); sid++) {
			_logger.log(2, "S%d disc %d\n", sid, passedFilter-added[sid]);
			if (!_params.isNotNull("fd")) {
				_logger.log(2, "S%d clear filter\n", sid);
				_solver_filters[sid].clear();	
			} 
		}
	}

	// Clear half of the clauses from the filter (probabilistically) if a clause filter half life is set
	if (_params.getIntParam("cfhl", 0) > 0 && _logger.getTime() - _last_buffer_clear > _params.getIntParam("cfhl", 0)) {
		_logger.log(1, "forget half of clauses in filters\n");
		for (size_t sid = 0; sid < _solver_filters.size(); sid++) {
			_solver_filters[sid].clearHalf();
		}
		_node_filter.clearHalf();
		_last_buffer_clear = _logger.getTime();
	}
}

void DefaultSharingManager::processClause(std::vector<int>& cls, int solverId) {

	_seen_clause_len_histogram[cls.size() == 1 ? 1 : std::min(cls.size(), (size_t)CLAUSE_LEN_HIST_LENGTH)-1]++;

	// If applicable, register clause in child filter
	// such that it will not be re-imported to this solver.
	if (_solvers.size() > 1) {
		_solver_filters[solverId].registerClause(cls);
	}

	// Check parent filter if this clause is admissible for export.
	// (If a clause is already registered, then we assume that it was, 
	// or will be, globally shared to everyone.)
	if (_node_filter.registerClause(cls)) {
		int* res = _cdb.addClause(cls);
		if (res == NULL) {
			_stats.clausesDroppedAtExport++;
		}
	} else {
		_stats.clausesFilteredAtExport++;
	}
}

SharingStatistics DefaultSharingManager::getStatistics() {
	return _stats;
}

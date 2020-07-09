/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include <assert.h>

#include "default_sharing_manager.hpp"

DefaultSharingManager::DefaultSharingManager(int mpi_size, int mpi_rank,
		vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const LoggingInterface& logger)
	:size(mpi_size),rank(mpi_rank),solvers(solvers),params(params),logger(logger),
	cdb(logger),nodeFilter(/*maxClauseLen=*/params.getIntParam("mcl", 0),/*checkUnits=*/true),callback(*this) {

    for (size_t i = 0; i < solvers.size(); i++) {
		if (solvers.size() > 1) {
			solverFilters.push_back(new ClauseFilter(/*maxClauseLen=*/params.getIntParam("mcl", 0), /*checkUnits=*/true));
		}
		solvers[i]->setLearnedClauseCallback(&callback);
	}
	lastBufferClear = logger.getTime();
}

int DefaultSharingManager::prepareSharing(int* begin, int maxSize) {

    //log(3, "Sharing clauses among %i nodes\n", size);
    static int prodInc = 1;
	static int lastInc = 0;
	if (!params.isSet("fd")) {
		logger.log(2, "clear node clsfltr\n");
		nodeFilter.clear();
	}
	int selectedCount;
	int used = cdb.giveSelection(begin, maxSize, &selectedCount);
	logger.log(3, "Prepared %i clauses, size %i\n", selectedCount, used);
	stats.sharedClauses += selectedCount;
	float usedRatio = ((float)used)/maxSize;
	if (usedRatio < params.isSet("icpr")) {
		int increaser = lastInc++ % solvers.size();
		solvers[increaser]->increaseClauseProduction();
		logger.log(3, "Node %d production increase for %d. time, sid %d will increase.\n", rank, prodInc++, increaser);
	}
	logger.log(3, "Filled %.1f%% of buffer\n", 100*usedRatio);
	return used;
}

void DefaultSharingManager::digestSharing(const std::vector<int>& result) {

	digestSharing(result.data(), result.size());
}

void DefaultSharingManager::digestSharing(const int* begin, int buflen) {
	
	// "size" is the amount of buffers in the result: 
	// always one, because buffers are merged into one big buffer
	size = 1;
    
    //std::memcpy(incommingBuffer, result.data(), result.size()*sizeof(int));
    
	//if (solvers.size() > 1) {
		// get all the clauses
		cdb.setIncomingBuffer(begin, buflen);
	//}
	/* else {
		// get all the clauses except for those that this node sent
		cdb.setIncomingBuffer(result.data(), COMM_BUFFER_SIZE, size, rank);
	}*/
	int passedFilter = 0;
	int failedFilter = 0;
	std::vector<int> lens;
	std::vector<int> added(solvers.size(), 0);

	int size;
	const int* clsbegin = cdb.getNextIncomingClause(size);
	while (clsbegin != NULL) {
		
		int clauseLen = (size > 1 ? size-1 : size); // subtract "glue" int
		while (clauseLen-1 >= lens.size()) lens.push_back(0);
		lens[clauseLen-1]++;

		if (nodeFilter.registerClause(clsbegin, size)) {
			
			// Add clause to solvers
			if (solvers.size() > 1) {
				for (size_t sid = 0; sid < solvers.size(); sid++) {
					if (solverFilters[sid]->registerClause(clsbegin, size)) {
						solvers[sid]->addLearnedClause(clsbegin, size);
						added[sid]++;
					}
				}
			} else {
				solvers[0]->addLearnedClause(clsbegin, size);
			}

			passedFilter++;
		} else {
			failedFilter++;
		}

		clsbegin = cdb.getNextIncomingClause(size);
	}
	int total = passedFilter + failedFilter;
	stats.filteredClauses += failedFilter;
	stats.importedClauses += passedFilter;
	
	if (total == 0) return;

	// Process-wide stats
	std::string lensStr = "";
	for (int len : lens) lensStr += std::to_string(len) + " ";
	logger.log(1, "fltrd %.2f%% (%d/%d), lens %s\n",
			100*(float)failedFilter/total, failedFilter, total, 
			lensStr.c_str());

	// Per-solver stats
	for (size_t sid = 0; sid < solvers.size(); sid++) {
		logger.log(2, "S%d fltrd %.2f%% (%d)\n", sid, passedFilter == 0 ? 0 : 100*(1-((float)added[sid]/passedFilter)), passedFilter-added[sid]);
		if (!params.isSet("fd")) {
			logger.log(2, "S%d clear clsfltr\n", sid);
			solverFilters[sid]->clear();	
		} 
	}

	// Clear half of the clauses from the filter (probabilistically) if a clause filter half life is set
	if (params.getIntParam("cfhl", 0) > 0 && logger.getTime() - lastBufferClear > params.getIntParam("cfhl", 0)) {
		logger.log(1, "forget half of clauses in filter\n");
		for (size_t sid = 0; sid < solverFilters.size(); sid++) {
			solverFilters[sid]->clearHalf();
		}
		nodeFilter.clearHalf();
		lastBufferClear = logger.getTime();
	}
}

SharingStatistics DefaultSharingManager::getStatistics() {
	return stats;
}

DefaultSharingManager::~DefaultSharingManager() {
	for (int i = 0; i < solverFilters.size(); i++) {
		delete solverFilters[i];
	}
}


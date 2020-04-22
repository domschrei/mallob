/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include <assert.h>

#include "AllToAllSharingManager.h"
#include "../utilities/mympi.h"

DefaultSharingManager::DefaultSharingManager(int mpi_size, int mpi_rank,
		vector<PortfolioSolverInterface*>& solvers, ParameterProcessor& params)
	:size(mpi_size),rank(mpi_rank),solvers(solvers),params(params),logger(params.getLogger()),
	cdb(logger),nodeFilter(/*maxClauseLen=*/params.getIntParam("mcl", 0),/*checkUnits=*/true),callback(*this) {
    for (size_t i = 0; i < solvers.size(); i++) {
		if (solvers.size() > 1) {
			solverFilters.push_back(new ClauseFilter(/*maxClauseLen=*/params.getIntParam("mcl", 0), /*checkUnits=*/true));
		}
		solvers[i]->setLearnedClauseCallback(&callback, i);
	}
}

std::vector<int> DefaultSharingManager::prepareSharing(int maxSize) {

    //log(3, "Sharing clauses among %i nodes\n", size);
    static int prodInc = 1;
	static int lastInc = 0;
	if (!params.isSet("fd")) {
		nodeFilter.clear();
	}
	int selectedCount;
	int used = cdb.giveSelection(outBuffer, maxSize, &selectedCount);
	logger.log(3, "Prepared %i clauses, size %i\n", selectedCount, used);
	stats.sharedClauses += selectedCount;
	float usedRatio = ((float)used)/maxSize;
	if (usedRatio < params.isSet("icpr")) {
		int increaser = lastInc++ % solvers.size();
		solvers[increaser]->increaseClauseProduction();
		logger.log(3, "Node %d production increase for %d. time, sid %d will increase.\n", rank, prodInc++, increaser);
	}
	logger.log(3, "Filled %.1f%% of buffer\n", 100*usedRatio);
    std::vector<int> clauseVec(outBuffer, outBuffer + used);

    return clauseVec;
}

void DefaultSharingManager::digestSharing(const std::vector<int>& result) {

	// "size" is the amount of buffers in the result: 
	// always one, because buffers are merged into one big buffer
	size = 1;
    
    //std::memcpy(incommingBuffer, result.data(), result.size()*sizeof(int));
    
	//if (solvers.size() > 1) {
		// get all the clauses
		cdb.setIncomingBuffer(result.data(), result.size());
	//}
	/* else {
		// get all the clauses except for those that this node sent
		cdb.setIncomingBuffer(result.data(), COMM_BUFFER_SIZE, size, rank);
	}*/
	vector<int> cl;
	int passedFilter = 0;
	int failedFilter = 0;
	long totalLen = 0;
	int minLen = result.size();
	int maxLen = 0;
	vector<vector<int> > clausesToAdd;
	while (cdb.getNextIncomingClause(cl)) {
		totalLen += cl.size();
		minLen = std::min(minLen, (int)cl.size());
		maxLen = std::max(maxLen, (int)cl.size());
		if (nodeFilter.registerClause(cl)) {
			clausesToAdd.push_back(cl);
			passedFilter++;
		} else {
			failedFilter++;
		}
	}
	int total = passedFilter + failedFilter;
	stats.filteredClauses += failedFilter;
	stats.importedClauses += passedFilter;
	
	if (total == 0) return;

	logger.log(1, "fltrd %.2f%% (%d/%d), min/avg/max %d/%.2f/%d\n",
			100*(float)failedFilter/total, failedFilter, total, 
			minLen, totalLen/(float)total, maxLen);

	if (solvers.size() > 1) {
		for (size_t sid = 0; sid < solvers.size(); sid++) {
			int added = 0;
			for (size_t cid = 0; cid < clausesToAdd.size(); cid++) {
				if (solverFilters[sid]->registerClause(clausesToAdd[cid])) {
					solvers[sid]->addLearnedClause(clausesToAdd[cid]);
					added++;
				}
			}
			logger.log(2, "S%d fltrd %.2f%% (%d)\n", sid, 100*(1-((float)added/clausesToAdd.size())), clausesToAdd.size()-added);
			if (!params.isSet("fd")) {
				solverFilters[sid]->clear();
			}
		}
	} else {
		solvers[0]->addLearnedClauses(clausesToAdd);
	}
}

SharingStatistics DefaultSharingManager::getStatistics() {
	return stats;
}

DefaultSharingManager::~DefaultSharingManager() {
	for (size_t i = 0; i < solverFilters.size(); i++) {
		delete solverFilters[i];
	}
}


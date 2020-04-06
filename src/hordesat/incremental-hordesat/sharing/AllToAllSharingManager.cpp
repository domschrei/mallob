/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include <assert.h>

#include "AllToAllSharingManager.h"
#include "../utilities/mympi.h"
#include "../utilities/Logger.h"


DefaultSharingManager::DefaultSharingManager(int mpi_size, int mpi_rank,
		vector<PortfolioSolverInterface*>& solvers, ParameterProcessor& params)
	:size(mpi_size),rank(mpi_rank),solvers(solvers),params(params),callback(*this) {
    for (size_t i = 0; i < solvers.size(); i++) {
		solvers[i]->setLearnedClauseCallback(&callback, i);
		if (solvers.size() > 1) {
			solverFilters.push_back(new ClauseFilter());
		}
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
	log(3, "Prepared %i clauses, size %i\n", selectedCount, used);
	stats.sharedClauses += selectedCount;
	int usedPercent = (100*used)/maxSize;
	if (usedPercent < 80) {
		int increaser = lastInc++ % solvers.size();
		solvers[increaser]->increaseClauseProduction();
		log(3, "Node %d production increase for %d. time, core %d will increase.\n", rank, prodInc++, increaser);
	}
	log(3, "Filled %d%% of buffer\n", usedPercent);
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
	vector<vector<int> > clausesToAdd;
	while (cdb.getNextIncomingClause(cl)) {
		totalLen += cl.size();
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
	if (total > 0) {
		log(2, "filtered %d%% (%d/%d), avg len %.2f\n",
				100*failedFilter/total,
				failedFilter, total, totalLen/(float)total);
	}
	for (size_t sid = 0; sid < solvers.size(); sid++) {
		solvers[sid]->addLearnedClauses(clausesToAdd);
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


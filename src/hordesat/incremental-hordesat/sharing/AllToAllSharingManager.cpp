/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include "AllToAllSharingManager.h"
#include "../utilities/mympi.h"
#include "../utilities/Logger.h"


AllToAllSharingManager::AllToAllSharingManager(int mpi_size, int mpi_rank,
		vector<PortfolioSolverInterface*> solvers, ParameterProcessor& params)
	:size(mpi_size),rank(mpi_rank),solvers(solvers),params(params),incommingBuffer(NULL),callback(*this) {
	init(size);
    for (size_t i = 0; i < solvers.size(); i++) {
		solvers[i]->setLearnedClauseCallback(&callback, i);
		if (solvers.size() > 1) {
			solverFilters.push_back(new ClauseFilter());
		}
	}
}

void AllToAllSharingManager::init(int size) {
    this->size = size;
    incommingBuffer = new int[(COMM_BUFFER_SIZE)*size];
}

std::vector<int> AllToAllSharingManager::prepareSharing(int size) {

    log(2, "Sharing clauses among %i nodes\n", size);
    init(size);
    static int prodInc = 1;
	static int lastInc = 0;
	if (!params.isSet("fd")) {
		nodeFilter.clear();
	}
	int selectedCount;
	int used = cdb.giveSelection(outBuffer, COMM_BUFFER_SIZE, &selectedCount);
	log(2, "Prepared %i clauses in a buffer of size %i\n", selectedCount, used);
	stats.sharedClauses += selectedCount;
	int usedPercent = (100*used)/COMM_BUFFER_SIZE;
	if (usedPercent < 80) {
		int increaser = lastInc++ % solvers.size();
		solvers[increaser]->increaseClauseProduction();
		log(2, "Node %d production increase for %d. time, core %d will increase.\n", rank, prodInc++, increaser);
	}
	log(0, "Node %d filled %d%% of its learned clause buffer\n", rank, usedPercent);
    std::vector<int> clauseVec(outBuffer, outBuffer + COMM_BUFFER_SIZE);

    /*
    std::string out;
    for (int i = 0; i < clauseVec.size(); i++) {
        out += std::to_string(i) + " ";
    }
    log(0, out + "\n");
    */

    return clauseVec;
}

void AllToAllSharingManager::doSharing() {

    prepareSharing(size);

	double barrierStart = getTime();
	if (params.isSet("barrier")) {
		MPI_Barrier(MPI_COMM_WORLD);
	}
	double barrierDone = getTime();
	double commStart = getTime();

	MPI_Allgather(outBuffer, COMM_BUFFER_SIZE, MPI_INT, incommingBuffer, COMM_BUFFER_SIZE, MPI_INT, MPI_COMM_WORLD);
	log(2, "Node %d allGather %.3f\n", rank, getTime() - commStart);

	if (params.isSet("barrier")) {
		MPI_Barrier(MPI_COMM_WORLD);
		log(2, "Node %d first barrier %.3f second barrier %.3f\n", rank, getTime() - barrierStart, getTime() - barrierDone);
	}

	digestSharing();
}

void AllToAllSharingManager::digestSharing(const std::vector<int>& result) {

    int newsize = result.size() / COMM_BUFFER_SIZE;
    init(newsize);

    std::memcpy(incommingBuffer, result.data(), result.size()*sizeof(int));
    digestSharing();
}

void AllToAllSharingManager::digestSharing() {

	if (solvers.size() > 1) {
		// get all the clauses
		cdb.setIncomingBuffer(incommingBuffer, COMM_BUFFER_SIZE, size, -1);
	} else {
		// get all the clauses except for those that this node sent
		cdb.setIncomingBuffer(incommingBuffer, COMM_BUFFER_SIZE, size, rank);
	}
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
	for (size_t sid = 0; sid < solvers.size(); sid++) {
		solvers[sid]->addLearnedClauses(clausesToAdd);
	}
	/*
	if (solvers.size() > 1) {
		for (size_t sid = 0; sid < solvers.size(); sid++) {
			for (size_t cid = 0; cid < clausesToAdd.size(); cid++) {
				if (solverFilters[sid]->registerClause(clausesToAdd[cid])) {
					solvers[sid]->addLearnedClause(clausesToAdd[cid]);
				}
			}
			if (!params.isSet("fd")) {
				solverFilters[sid]->clear();
			}
		}
	} else {
		solvers[0]->addLearnedClauses(clausesToAdd);
	}
	*/
	int total = passedFilter + failedFilter;
	stats.filteredClauses += failedFilter;
	stats.importedClauses += passedFilter;
	if (total > 0) {
		log(2, "filter blocked %d%% (%d/%d) of incomming clauses, avg len %.2f\n",
				100*failedFilter/total,
				failedFilter, total, totalLen/(float)total);
	}

}

SharingStatistics AllToAllSharingManager::getStatistics() {
	return stats;
}

AllToAllSharingManager::~AllToAllSharingManager() {
	for (size_t i = 0; i < solverFilters.size(); i++) {
		delete solverFilters[i];
	}
	delete[] incommingBuffer;
}


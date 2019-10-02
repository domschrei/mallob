/*
 * LogSharingManager.cpp
 *
 *  Created on: Apr 1, 2015
 *      Author: balyo
 */

#include "LogSharingManager.h"
#include "../utilities/mympi.h"
#include "../utilities/Logger.h"

LogSharingManager::LogSharingManager(int mpi_size, int mpi_rank, vector<PortfolioSolverInterface*> solvers,
			ParameterProcessor& params):AllToAllSharingManager(mpi_size, mpi_rank, solvers, params) {
	init(mpi_size);
}

void LogSharingManager::init(int size) {
    this->size = size;
    exchangeCount = 0;
	int tsize = size;
	while (tsize >>= 1) {
		exchangeCount++;
	}
	log(2, "Clause exchange partners: %d (log(%d))\n", exchangeCount, size);
	if (exchangeCount > 0) {
		delete[] incommingBuffer;
		incommingBuffer = new int[COMM_BUFFER_SIZE*exchangeCount];
	}
}

std::vector<int> LogSharingManager::prepareSharing(int size) {

    init(size);
    static int prodInc = 1;
	static int lastInc = 0;
	if (!params.isSet("fd")) {
		nodeFilter.clear();
	}
	int selectedCount;
	int used = cdb.giveSelection(outBuffer, COMM_BUFFER_SIZE, &selectedCount);
	stats.sharedClauses += selectedCount;
	int usedPercent = (100*used)/COMM_BUFFER_SIZE;
	if (usedPercent < 80) {
		int increaser = lastInc++ % solvers.size();
		solvers[increaser]->increaseClauseProduction();
		log(2, "Node %d production increase for %d. time, core %d will increase.\n", rank, prodInc++, increaser);
	}
	log(2, "Node %d filled %d%% of its learned clause buffer\n", rank, usedPercent);

}

void LogSharingManager::digestSharing(const std::vector<int>& result) {
    std::memcpy(incommingBuffer, result.data(), sizeof result.data());
    digestSharing();
}

void LogSharingManager::digestSharing() {

    cdb.setIncomingBuffer(incommingBuffer, COMM_BUFFER_SIZE, exchangeCount, -1);
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
	int total = passedFilter + failedFilter;
	stats.filteredClauses += failedFilter;
	stats.importedClauses += passedFilter;
	if (total > 0) {
		log(2, "filter blocked %d%% (%d/%d) of incomming clauses, avg len %.2f\n",
				100*failedFilter/total,
				failedFilter, total, totalLen/(float)total);
	}
}

void LogSharingManager::doSharing() {

    prepareSharing(size);

	static int round = 0;
	for (int i = 0; i < exchangeCount; i++) {
		int partner = (round - rank + size) % size;
		if (partner == rank && (size % 2 == 0)) {
			partner = (partner + size/2) % size;
		}
		round++;
		log(2, "Clause exchange between %d and %d\n", rank, partner);
		MPI_Sendrecv(outBuffer,                            COMM_BUFFER_SIZE, MPI_INT, partner, 0,
                     incommingBuffer + i*COMM_BUFFER_SIZE, COMM_BUFFER_SIZE, MPI_INT, partner, 0,
                     MPI_COMM_WORLD, 0);
	}
	if (exchangeCount == 0) {
		memcpy(incommingBuffer, outBuffer, sizeof(int)*COMM_BUFFER_SIZE);
	}

	digestSharing();
}

LogSharingManager::~LogSharingManager() {
}


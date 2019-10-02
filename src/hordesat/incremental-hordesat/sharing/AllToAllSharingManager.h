/*
 * AllToAllSharingManager.h
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#ifndef SHARING_ALLTOALLSHARINGMANAGER_H_
#define SHARING_ALLTOALLSHARINGMANAGER_H_

#include <cstring>

#include "SharingManagerInterface.h"
#include "../utilities/ClauseDatabase.h"
#include "../utilities/ClauseFilter.h"
#include "../utilities/ParameterProcessor.h"

#define COMM_BUFFER_SIZE 1500

class AllToAllSharingManager : public SharingManagerInterface {

protected:
	// MPI paramaters
	int size, rank;
	// associated solvers
	vector<PortfolioSolverInterface*> solvers;
	vector<ClauseFilter*> solverFilters;
	// global parameters
	ParameterProcessor& params;

	ClauseDatabase cdb;
	ClauseFilter nodeFilter;
	int outBuffer[COMM_BUFFER_SIZE];
	int* incommingBuffer;

	class Callback : public LearnedClauseCallback {
	public:
		AllToAllSharingManager& parent;
		Callback(AllToAllSharingManager& parent):parent(parent) {
		}
		void processClause(vector<int>& cls, int solverId) {
			if (parent.solvers.size() > 1) {
				parent.solverFilters[solverId]->registerClause(cls);
			}
			if (parent.nodeFilter.registerClause(cls)) {
				int* res = parent.cdb.addClause(cls);
				if (res == NULL) {
					parent.stats.dropped++;
				}
			} else {
				parent.stats.filteredClauses++;
			}
		}
	};

	Callback callback;
	SharingStatistics stats;

public:
	AllToAllSharingManager(int mpi_size, int mpi_rank, vector<PortfolioSolverInterface*> solvers,
			ParameterProcessor& params);
	void init(int size);
    void doSharing();
    std::vector<int> prepareSharing(int size);
    void digestSharing();
    void digestSharing(const std::vector<int>& result);
	SharingStatistics getStatistics();
	~AllToAllSharingManager();
};

#endif /* SHARING_ALLTOALLSHARINGMANAGER_H_ */

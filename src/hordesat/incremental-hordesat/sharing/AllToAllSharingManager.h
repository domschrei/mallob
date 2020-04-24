/*
 * AllToAllSharingManager.h
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#ifndef SHARING_ALLTOALLSHARINGMANAGER_H_
#define SHARING_ALLTOALLSHARINGMANAGER_H_

#include <cstring>
#include <memory>

#include "SharingManagerInterface.h"
#include "../utilities/ClauseDatabase.h"
#include "../utilities/ClauseFilter.h"
#include "../utilities/ParameterProcessor.h"

#define COMM_BUFFER_SIZE 1500

class DefaultSharingManager : public SharingManagerInterface {

protected:
	// MPI paramaters
	int size, rank;
	// associated solvers
	vector<std::shared_ptr<PortfolioSolverInterface>>& solvers;
	vector<ClauseFilter*> solverFilters;
	// global parameters
	ParameterProcessor& params;
	LoggingInterface& logger;

	ClauseDatabase cdb;
	ClauseFilter nodeFilter;
	int outBuffer[COMM_BUFFER_SIZE];

	class Callback : public LearnedClauseCallback {
	public:
		DefaultSharingManager& parent;
		Callback(DefaultSharingManager& parent):parent(parent) {
		}
		void processClause(vector<int>& cls, int solverId) {
			//parent.logger.log(3, "process clause\n");
			if (parent.solvers.size() > 1) {
				//parent.logger.log(3, "register clause in child\n");
				parent.solverFilters[solverId]->registerClause(cls);
			}
			//parent.logger.log(3, "register clause in parent\n");
			if (parent.nodeFilter.registerClause(cls)) {
				//parent.logger.log(3, "registered successfully in parent\n");
				int* res = parent.cdb.addClause(cls);
				if (res == NULL) {
					parent.stats.dropped++;
				}
			} else {
				//parent.logger.log(3, "not registered in parent\n");
				parent.stats.filteredClauses++;
			}
		}
	};

	Callback callback;
	SharingStatistics stats;

public:
	DefaultSharingManager(int mpi_size, int mpi_rank, vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
			ParameterProcessor& params);
    std::vector<int> prepareSharing(int maxSize);
    void digestSharing(const std::vector<int>& result);
	SharingStatistics getStatistics();
	~DefaultSharingManager();
};

#endif /* SHARING_ALLTOALLSHARINGMANAGER_H_ */

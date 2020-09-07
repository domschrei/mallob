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

#include "app/sat/hordesat/sharing/sharing_manager_interface.hpp"
#include "app/sat/hordesat/utilities/clause_database.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp"
#include "util/params.hpp"

#define COMM_BUFFER_SIZE 1500

class DefaultSharingManager : public SharingManagerInterface {

protected:
	// MPI paramaters
	int size, rank;
	// associated solvers
	vector<std::shared_ptr<PortfolioSolverInterface>>& solvers;
	vector<ClauseFilter*> solverFilters;
	
	// global parameters
	const Parameters& params;
	const LoggingInterface& logger;

	ClauseDatabase cdb;
	ClauseFilter nodeFilter;
	int outBuffer[COMM_BUFFER_SIZE];

	float lastBufferClear = 0;

	class Callback : public LearnedClauseCallback {
	public:
		DefaultSharingManager& parent;
		bool hasSolverFilters;
		Callback(DefaultSharingManager& parent):parent(parent) {
			hasSolverFilters = parent.solvers.size() > 1;
		}
		void processClause(vector<int>& cls, int solverId) {

			// If applicable, register clause in child filter
			// such that it will not be re-imported to this solver.
			if (hasSolverFilters) {
				parent.solverFilters[solverId]->registerClause(cls);
			}

			// Check parent filter if this clause is admissible for export.
			// (If a clause is already registered, then we assume that it was, 
			// or will be, globally shared to everyone.)
			if (parent.nodeFilter.registerClause(cls)) {
				int* res = parent.cdb.addClause(cls);
				if (res == NULL) {
					parent.stats.clausesDroppedAtExport++;
				}
			} else {
				parent.stats.clausesFilteredAtExport++;
			}
		}
	};

	Callback callback;
	SharingStatistics stats;

public:
	DefaultSharingManager(int mpi_size, int mpi_rank, vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
			const Parameters& params, const LoggingInterface& logger);
    int prepareSharing(int* begin, int maxSize);
    void digestSharing(const std::vector<int>& result);
	void digestSharing(const int* begin, int buflen);
	SharingStatistics getStatistics();
	~DefaultSharingManager();
};

#endif /* SHARING_ALLTOALLSHARINGMANAGER_H_ */

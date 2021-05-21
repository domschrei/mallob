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
#include "app/sat/hordesat/sharing/lockfree_clause_database.hpp"
#include "app/sat/hordesat/utilities/clause.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp"
#include "util/params.hpp"

#define CLAUSE_LEN_HIST_LENGTH 256

class DefaultSharingManager : public SharingManagerInterface {

protected:
	// associated solvers
	std::vector<std::shared_ptr<PortfolioSolverInterface>>& _solvers;
	std::vector<ClauseFilter> _solver_filters;
	
	// global parameters
	const Parameters& _params;
	const Logger& _logger;

	LockfreeClauseDatabase _cdb;
	
	float _last_buffer_clear = 0;

	unsigned long _seen_clause_len_histogram[CLAUSE_LEN_HIST_LENGTH];

	SharingStatistics _stats;

public:
	DefaultSharingManager(std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
			const Parameters& params, const Logger& logger);
    int prepareSharing(int* begin, int maxSize);
    void digestSharing(std::vector<int>& result);
	void digestSharing(int* begin, int buflen);
	SharingStatistics getStatistics();
	~DefaultSharingManager() = default;

private:
	void processClause(int solverId, const Clause& clause);

};

#endif /* SHARING_ALLTOALLSHARINGMANAGER_H_ */

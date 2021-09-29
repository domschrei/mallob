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
#include <list>

#include "app/sat/hordesat/sharing/sharing_manager_interface.hpp"
#include "app/sat/hordesat/utilities/clause_database.hpp"
#include "app/sat/hordesat/sharing/adaptive_clause_database.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp"
#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "util/params.hpp"

#define CLAUSE_LEN_HIST_LENGTH 256

class DefaultSharingManager : public SharingManagerInterface {

protected:
	// associated solvers
	std::vector<std::shared_ptr<PortfolioSolverInterface>>& _solvers;

	// The process-wide clause filter has the purpose of preventing 
	// duplicate clauses in the database of exported clauses.
	ClauseFilter _process_filter;
	// Each solver clause filter has the purpose of preventing the 
	// solver's own clauses being mirrored back to itself.
	std::vector<ClauseFilter> _solver_filters;
	
	std::vector<int> _solver_revisions;

	// clause export
	std::vector<std::list<Clause>> _deferred_admitted_clauses;

	SolvingStatistics _returned_clauses_stats;
	std::list<Clause> _deferred_returned_clauses;

	// clause importing / digestion
	struct DeferredClauseList {
		int revision;
		size_t numLits = 0;
		std::vector<bool> involvedSolvers;
		std::vector<int> buffer;
		std::vector<Mallob::Clause> clauses;
	};
	std::list<DeferredClauseList> _future_clauses;
	size_t _max_deferred_lits_per_solver;
	
	// global parameters
	const Parameters& _params;
	const Logger& _logger;
	int _job_index;

	AdaptiveClauseDatabase _cdb;
	
	float _last_buffer_clear = 0;

	ClauseHistogram _hist_produced;
	ClauseHistogram _hist_admitted_to_db;
	ClauseHistogram _hist_dropped_before_db;
	ClauseHistogram _hist_returned_to_db;

	SharingStatistics _stats;
	std::vector<SolvingStatistics*> _solver_stats;

	int _current_revision = -1;

public:
	DefaultSharingManager(std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
			const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver,
			int jobIndex);
	~DefaultSharingManager() = default;

    int prepareSharing(int* begin, int maxSize);
    void returnClauses(int* begin, int buflen);

	void digestSharing(std::vector<int>& result);
	void digestSharing(int* begin, int buflen);

	SharingStatistics getStatistics();

	void setRevision(int revision) override {_current_revision = revision;}
	void stopClauseImport(int solverId) override;
	void continueClauseImport(int solverId) override;

private:
	void processClause(int solverId, int solverRevision, const Clause& clause, int condVarOrZero);
	ExtLearnedClauseCallback getCallback() {
		return [this](const Clause& c, int solverId, int solverRevision, int condVarOrZero) {
			processClause(solverId, solverRevision, c, condVarOrZero);
		};
	};

	void tryReinsertDeferredClauses(int solverId, std::list<Clause>& clauses, SolvingStatistics* stats);
	void digestDeferredFutureClauses();


};

#endif /* SHARING_ALLTOALLSHARINGMANAGER_H_ */


#ifndef DOMPASCH_MALLOB_SOLVER_STATISTICS_HPP
#define DOMPASCH_MALLOB_SOLVER_STATISTICS_HPP

#include <string>

#include "app/sat/hordesat/sharing/clause_histogram.hpp"

struct SolvingStatistics {

	unsigned long propagations = 0;
	unsigned long decisions = 0;
	unsigned long conflicts = 0;
	unsigned long restarts = 0;
	double memPeak = 0;
	
	// clause export
	ClauseHistogram* histProduced;
	unsigned long producedClauses = 0;
	unsigned long producedClausesProcessFiltered = 0;
	unsigned long producedClausesSolverFiltered = 0;
	unsigned long producedClausesDeferred = 0;
	unsigned long producedClausesAdmitted = 0;
	unsigned long producedClausesDropped = 0;

	// clause import
	ClauseHistogram* histDigested;
	unsigned long receivedClauses = 0;
	unsigned long receivedClausesInserted = 0;
	unsigned long receivedClausesFiltered = 0;
	unsigned long deferredClauses = 0;
	unsigned long digestedClauses = 0;
	unsigned long discardedClauses = 0;

	std::string getReport() const {
		return "pps:" + std::to_string(propagations)
			+ " dcs:" + std::to_string(decisions)
			+ " cfs:" + std::to_string(conflicts)
			+ " rst:" + std::to_string(restarts)
			+ " prod:" + std::to_string(producedClauses)
			+ " admd:" + std::to_string(producedClausesAdmitted)
			+ " pfltr:" + std::to_string(producedClausesProcessFiltered)
			+ " sfltr:" + std::to_string(producedClausesSolverFiltered)
			+ " pdefr:" + std::to_string(producedClausesDeferred)
			+ " drpd:" + std::to_string(producedClausesDropped)
			+ " recv:" + std::to_string(receivedClauses)
			+ " rfltr:" + std::to_string(receivedClausesFiltered)
			+ " digd:" + std::to_string(digestedClauses)
			+ " defr:" + std::to_string(deferredClauses)
			+ " disc:" + std::to_string(discardedClauses);
	}

	void aggregate(const SolvingStatistics& other) {
		propagations += other.propagations;
		decisions += other.decisions;
		conflicts += other.conflicts;
		restarts += other.restarts;
		memPeak += other.memPeak;
		producedClauses += other.producedClauses;
		producedClausesAdmitted += other.producedClausesAdmitted;
		producedClausesProcessFiltered += other.producedClausesProcessFiltered;
		producedClausesSolverFiltered += other.producedClausesSolverFiltered;
		producedClausesDropped += other.producedClausesDropped;
		receivedClauses += other.receivedClauses;
		receivedClausesFiltered += other.receivedClausesFiltered;
		deferredClauses += other.deferredClauses;
		digestedClauses += other.digestedClauses;
		discardedClauses += other.discardedClauses;
	}
};

#endif

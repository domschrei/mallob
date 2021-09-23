
#ifndef DOMPASCH_MALLOB_SOLVER_STATISTICS_HPP
#define DOMPASCH_MALLOB_SOLVER_STATISTICS_HPP

#include <string>

struct SolvingStatistics {
	unsigned long propagations = 0;
	unsigned long decisions = 0;
	unsigned long conflicts = 0;
	unsigned long restarts = 0;
	double memPeak = 0;
	
	// clause export
	unsigned long producedClauses = 0;
	unsigned long producedClausesAdmitted = 0;
	unsigned long producedClausesFiltered = 0;
	unsigned long producedClausesDropped = 0;

	// clause import
	unsigned long receivedClauses = 0;
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
			+ " padmd:" + std::to_string(producedClausesAdmitted)
			+ " pfltr:" + std::to_string(producedClausesFiltered)
			+ " pdrpd:" + std::to_string(producedClausesDropped)
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
		producedClausesFiltered += other.producedClausesFiltered;
		producedClausesDropped += other.producedClausesDropped;
		receivedClauses += other.receivedClauses;
		receivedClausesFiltered += other.receivedClausesFiltered;
		deferredClauses += other.deferredClauses;
		digestedClauses += other.digestedClauses;
		discardedClauses += other.discardedClauses;
	}
};

#endif

/*
 * portfolioSolverInterface.h
 *
 *  Created on: Oct 10, 2014
 *      Author: balyo
 */

#ifndef PORTFOLIOSOLVERINTERFACE_H_
#define PORTFOLIOSOLVERINTERFACE_H_

#include <vector>
#include <set>
#include <stdexcept>

#include "../utilities/logging_interface.h"

using namespace std;

enum SatResult {
	SAT = 10,
	UNSAT = 20,
	UNKNOWN = 0
};

struct SolvingStatistics {
	SolvingStatistics():propagations(0),decisions(0),conflicts(0),restarts(0),memPeak(0) {}
	unsigned long propagations;
	unsigned long decisions;
	unsigned long conflicts;
	unsigned long restarts;
	double memPeak;
};

class LearnedClauseCallback {
public:
	virtual void processClause(vector<int>& cls, int solverId) = 0;
	virtual ~LearnedClauseCallback() {};
};

/**
 * Interface for solvers that can be used used in the portfolio
 */
class PortfolioSolverInterface {

protected:
	LoggingInterface& _logger;

public:
	int solverId;
	std::string _global_name;

	// constructor
	PortfolioSolverInterface(LoggingInterface& logger) : _logger(logger) {}

    // destructor
	virtual ~PortfolioSolverInterface() {}

	void setName(std::string name) {_global_name = name;}

	// Get the number of variables of the formula
	// NOT NECESSARY
	virtual int getVariablesCount() = 0;

	// Get a variable suitable for search splitting
	// NOT NECESSARY
	virtual int getSplittingVariable() = 0;

	// Set initial phase for a given variable
	// NOT NECESSARY, used only for diversification of the portfolio
	virtual void setPhase(const int var, const bool phase) = 0;

	// Interrupt the SAT solving, solving cannot continue until interrupt is unset.
	virtual void setSolverInterrupt() = 0;

	// Remove the SAT solving interrupt request.
	virtual void unsetSolverInterrupt() = 0;

    // Suspend the SAT solver DURING its execution, freeing up computational resources for other threads
    virtual void setSolverSuspend() = 0;

	// Remove the SAT solving suspend request
    virtual void unsetSolverSuspend() = 0;

	// Solve the formula with a given set of assumptions
	virtual SatResult solve(const vector<int>& assumptions = vector<int>()) = 0;

	virtual vector<int> getSolution() = 0;
	virtual set<int> getFailedAssumptions() = 0;

	// Add a permanent literal to the formula (zero for clause separator)
	virtual void addLiteral(int lit) = 0;

	// Add a learned clause to the formula
	// The learned clauses might be added later or possibly never
	virtual void addLearnedClause(const int* begin, int size) = 0;

	// Set a function that should be called for each learned clause
	virtual void setLearnedClauseCallback(LearnedClauseCallback* callback, int solverId) = 0;

	// Request the solver to produce more clauses
	virtual void increaseClauseProduction() = 0;

	// Get solver statistics
	// NOT NECESSARY
	virtual SolvingStatistics getStatistics() = 0;

	// You are solver #rank of #size solvers, diversify your parameters (seeds, heuristics, etc.) accordingly.
	virtual void diversify(int rank, int size) = 0;

	// How many "true" different diversifications do you have?
	// May be used to decide when to apply additional diversifications.
	virtual int getNumOriginalDiversifications() = 0;


	friend void slog(PortfolioSolverInterface* slv, int verbosityLevel, const char* fmt, ...);
};

void updateTimer(std::string solverName);
double getTime();
void slog(PortfolioSolverInterface* slv, int verbosityLevel, const char* fmt, ...);

#endif /* PORTFOLIOSOLVERINTERFACE_H_ */

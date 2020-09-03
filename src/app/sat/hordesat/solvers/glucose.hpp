/*
 * Lingeling.h
 *
 *  Created on: Nov 11, 2014
 *      Author: balyo
 */

#ifndef GLUCOSE_H_
#define GLUCOSE_H_

#include "portfolio_solver_interface.hpp"
#include "util/sys/threading.hpp"
#include "app/sat/hordesat/utilities/logging_interface.hpp"

#include "app/sat/hordesat/glucose/simp/SimpSolver.h"

#include <map>

class MGlucose : Glucose::SimpSolver, public PortfolioSolverInterface {

private:
	std::string name;
	int stopSolver;
	LearnedClauseCallback* learnedClauseCallback;
	unsigned int glueLimit;
	Mutex clauseAddMutex;

	int maxvar;
	Glucose::vec<Glucose::Lit> clause;
	Glucose::vec<Glucose::Lit> assumptions;
	int szfmap; 
	unsigned char * fmap; 
	bool nomodel;
	unsigned long long calls;
    
	// Friends: Callbacks for Lingeling and logging inside these callbacks
	friend void slog(PortfolioSolverInterface* slv, int verbosityLevel, const char* fmt, ...);

	// clause addition
	vector<vector<int> > clausesToAdd;
	vector<vector<int> > learnedClausesToAdd;
	vector<int> unitsToAdd;
	int* unitsBuffer;
	size_t unitsBufferSize;
	int* clsBuffer;
	size_t clsBufferSize;
    
    volatile bool suspendSolver;
    Mutex suspendMutex;
    ConditionVariable suspendCond;

	int numDiversifications;

public:
	MGlucose(LoggingInterface& logger, int globalId, int localId, std::string jobName);
	 ~MGlucose() override;

	// Add a (list of) permanent clause(s) to the formula
	void addLiteral(int lit) override;

	void diversify(int rank, int size) override;
	void setPhase(const int var, const bool phase) override;

	// Solve the formula with a given set of assumptions
	SatResult solve(const vector<int>& assumptions) override;

	void setSolverInterrupt() override;
	void unsetSolverInterrupt() override;
    void setSolverSuspend() override;
    void unsetSolverSuspend() override;

	vector<int> getSolution() override;
	set<int> getFailedAssumptions() override;

	// Add a learned clause to the formula
	// The learned clauses might be added later or possibly never
	void addLearnedClause(const int* begin, int size) override;

	// Set a function that should be called for each learned clause
	void setLearnedClauseCallback(LearnedClauseCallback* callback) override;

	// Request the solver to produce more clauses
	void increaseClauseProduction() override;
	
	// Get the number of variables of the formula
	int getVariablesCount() override;

	int getNumOriginalDiversifications() override;
	
	// Get a variable suitable for search splitting
	int getSplittingVariable() override;

	// Get solver statistics
	SolvingStatistics getStatistics() override;

private:
	Glucose::Lit encodeLit(int lit);
	int decodeLit(Glucose::Lit lit);
	void resetMaps();
	int solvedValue(int lit);
	bool failed(Glucose::Lit lit);
	void buildFailedMap();

	bool parallelJobIsFinished() override;

    void parallelImportUnaryClauses() override;
    bool parallelImportClauses() override; // true if the empty clause was received

	void parallelImportClauseDuringConflictAnalysis(Glucose::Clause &c, Glucose::CRef confl) override;
    
	void parallelExportUnaryClause(Glucose::Lit p) override;
    void parallelExportClauseDuringSearch(Glucose::Clause &c) override;
    
};

#endif /* LINGELING_H_ */

/*
 * Lingeling.h
 *
 *  Created on: Nov 11, 2014
 *      Author: balyo
 */

#ifndef LINGELING_H_
#define LINGELING_H_

#include "portfolio_solver_interface.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"

#include "util/ringbuffer.hpp"

struct LGL;

class Lingeling : public PortfolioSolverInterface {

private:
	LGL* solver;
	std::string name;
	int stopSolver;
	LearnedClauseCallback callback;
	int maxvar;
	double lastTermCallbackTime;
	const bool incremental;
    
	// Friends: Callbacks for Lingeling and logging inside these callbacks
	friend int cbCheckTerminate(void* solverPtr);
	friend void cbProduce(void* sp, int* cls, int glue);
	friend void cbProduceUnit(void* sp, int lit);
	friend void cbConsumeUnits(void* sp, int** start, int** end);
	friend void cbConsumeCls(void* sp, int** clause, int* glue);

	// clause addition
	std::vector<int> assumptions;

	MixedNonunitClauseRingBuffer learnedClauses;
	UnitClauseRingBuffer learnedUnits;
	
	std::vector<int> learnedClausesBuffer;
	std::vector<int> learnedUnitsBuffer;
	std::vector<int> learnedClause;

	unsigned long numReceived = 0;
	unsigned long numDigested = 0;
	unsigned long numDiscarded = 0;
    
    volatile bool suspendSolver;
    Mutex suspendMutex;
    ConditionVariable suspendCond;

	unsigned int numDiversifications;
	unsigned int glueLimit;
	unsigned int sizeLimit;

	void doProduce(int* cls, int glue);
	void doProduceUnit(int lit);
	void doConsume(int** start, int* glue);
	void doConsumeUnits(int** start, int** end);

public:
	Lingeling(const SolverSetup& setup);
	 ~Lingeling() override;

	// Add a (list of) permanent clause(s) to the formula
	void addLiteral(int lit) override;

	void diversify(int seed) override;
	void setPhase(const int var, const bool phase) override;

	// Solve the formula with a given set of assumptions
	SatResult solve(size_t numAssumptions, const int* assumptions) override;

	void setSolverInterrupt() override;
	void unsetSolverInterrupt() override;
    void setSolverSuspend() override;
    void unsetSolverSuspend() override;

	std::vector<int> getSolution() override;
	std::set<int> getFailedAssumptions() override;

	// Add a learned clause to the formula
	// The learned clauses might be added later or possibly never
	void addLearnedClause(const Clause& clause) override;

	// Set a function that should be called for each learned clause
	void setLearnedClauseCallback(const LearnedClauseCallback& callback) override;

	// Request the solver to produce more clauses
	void increaseClauseProduction() override;
	
	// Get the number of variables of the formula
	int getVariablesCount() override;

	int getNumOriginalDiversifications() override;
	
	// Get a variable suitable for search splitting
	int getSplittingVariable() override;

	// Get solver statistics
	SolvingStatistics getStatistics() override;

	bool supportsIncrementalSat() override {return true;}
	bool exportsConditionalClauses() override {return false;}

private:
	void updateMaxVar(int lit);
    
};

#endif /* LINGELING_H_ */

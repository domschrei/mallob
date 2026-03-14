
#pragma once

#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"

class IPAsirMiniSAT;

class MiniSat : public PortfolioSolverInterface {

public:
    MiniSat(const SolverSetup& setup);
    virtual ~MiniSat();

    // Get the number of variables of the formula
	virtual int getVariablesCount() override;
	// Get a variable suitable for search splitting
	virtual int getSplittingVariable() override;
	// Set initial phase for a given variable
	// Used only for diversification of the portfolio
	virtual void setPhase(const int var, const bool phase) override;
	// Solve the formula with a given set of assumptions
	virtual SatResult solve(size_t numAssumptions, const int* assumptions) override;
	// Get a solution vector containing lit or -lit for each lit in the model
	virtual std::vector<int> getSolution() override;
	// Get a set of failed assumptions
	virtual std::set<int> getFailedAssumptions() override;
	virtual unsigned long getUnsatConclusionId() const override {return 0;}

	// Add a permanent literal to the formula (zero for clause separator)
	virtual void addLiteral(int lit) override;
	// Set a function that should be called for each learned clause
	virtual void setLearnedClauseCallback(const LearnedClauseCallback& callback) override;
	// Set a function that can be called to probe whether a clause of specified length
	// may be eligible for export. (It might still be rejected upon export.)
	virtual void setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& callback) override;
	// Get solver statistics
	virtual void writeStatistics(SolverStatistics& stats) override;
	// Diversify your parameters (seeds, heuristics, etc.) according to the seed
	// and the individual diversification index given by getDiversificationIndex().
	virtual void diversify(int seed) override;
	// How many "true" different diversifications do you have?
	// May be used to decide when to apply additional diversifications.
	virtual int getNumOriginalDiversifications() override;
	virtual bool supportsIncrementalSat() override;
	virtual bool exportsConditionalClauses() override;
	virtual void cleanUp() override;

protected:
	// Interrupt the SAT solving, solving cannot continue until interrupt is unset.
	virtual void setSolverInterrupt() override;
	// Resume SAT solving after it was interrupted.
	virtual void unsetSolverInterrupt() override;

private:
	std::vector<int> _assumptions;
	IPAsirMiniSAT* _solver {0};
	volatile bool interrupt {false};

	friend int cbTerminate(void * state);
};

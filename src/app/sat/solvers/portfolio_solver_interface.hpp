
#pragma once

#include <stddef.h>
#include <vector>
#include <set>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "app/sat/data/definitions.hpp"
#include "app/sat/data/solver_statistics.hpp"
#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/sharing/generic_import_manager.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "app/sat/solvers/optimizing_propagator.hpp"
#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/string_utils.hpp"

class BufferReader;
namespace Mallob {
struct Clause;
}  // namespace Mallob

void updateTimer(std::string jobName);

class LratConnector; // fwd

/**
 * Interface for solvers that can be used in the portfolio.
 */
class PortfolioSolverInterface {

protected:
	Logger _logger;
	SolverSetup _setup;
	LratConnector* _lrat {nullptr};
	std::unique_ptr<OptimizingPropagator> _optimizer;

// ************** INTERFACE TO IMPLEMENT **************

public:
	// constructor
	PortfolioSolverInterface(const SolverSetup& setup);

    // destructor
	virtual ~PortfolioSolverInterface();

	// Get the number of variables of the formula
	virtual int getVariablesCount() = 0;

	// Get a variable suitable for search splitting
	virtual int getSplittingVariable() = 0;

	// Set initial phase for a given variable
	// Used only for diversification of the portfolio
	virtual void setPhase(const int var, const bool phase) = 0;

	// Solve the formula with a given set of assumptions
	virtual SatResult solve(size_t numAssumptions, const int* assumptions) = 0;

	// Get a solution vector containing lit or -lit for each lit in the model
	virtual std::vector<int> getSolution() = 0;

	// Get a set of failed assumptions
	virtual std::set<int> getFailedAssumptions() = 0;

	// Add a permanent literal to the formula (zero for clause separator)
	virtual void addLiteral(int lit) = 0;

	// Set a function that should be called for each learned clause
	virtual void setLearnedClauseCallback(const LearnedClauseCallback& callback) = 0;

	// Set a function that can be called to probe whether a clause of specified length
	// may be eligible for export. (It might still be rejected upon export.)
	virtual void setProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& callback) {}

	// Get solver statistics
	virtual void writeStatistics(SolverStatistics& stats) = 0;

	// Diversify your parameters (seeds, heuristics, etc.) according to the seed
	// and the individual diversification index given by getDiversificationIndex().
	virtual void diversify(int seed) = 0;

	// How many "true" different diversifications do you have?
	// May be used to decide when to apply additional diversifications.
	virtual int getNumOriginalDiversifications() = 0;

	virtual bool supportsIncrementalSat() = 0;
	virtual bool exportsConditionalClauses() = 0;

	virtual void cleanUp() = 0;

protected:
	// Interrupt the SAT solving, solving cannot continue until interrupt is unset.
	virtual void setSolverInterrupt() = 0;

	// Resume SAT solving after it was interrupted.
	virtual void unsetSolverInterrupt() = 0;

// ************** END OF INTERFACE TO IMPLEMENT **************


// Other methods

public:
	/**
	 * The solver's ID which is globally unique for the particular job
	 * that is being computed on.
	 * Equal to <rank> * <solvers_per_node> + <local_id>.
	 */
	int getGlobalId() {return _global_id;}
	/**
	 * The solver's local ID on this node and job. 
	 */
	int getLocalId() {return _local_id;}
	/**
	 * This number n denotes that this solver is the n-th solver of this type
	 * being employed to compute on this job.
	 * Equal to the global ID minus the number of solvers of a different type.
	 */
	int getDiversificationIndex() {return _diversification_index;}
	
	void setClauseSharing(int numOriginalDiversifications) {
		// Skip clause sharing occasionally after original diversification is exhausted
		if (_setup.skipClauseSharingDiagonally && getDiversificationIndex() >= numOriginalDiversifications) {
			int depth = getDiversificationIndex() / numOriginalDiversifications;
			int divCycleIdx = getDiversificationIndex() % numOriginalDiversifications;
			if (divCycleIdx+1 == depth) {
				LOGGER(_logger, V4_VVER, "Skip clause sharing\n");
				_clause_sharing_disabled = true;
			}
		}
	}
	bool isClauseSharingEnabled() const {
		return !_clause_sharing_disabled;
	}

	void addConditionalLit(int condLit) {assert(condLit != 0); _conditional_lits.push_back(condLit);}
	void clearConditionalLits() {_conditional_lits.clear();}
	void setExtLearnedClauseCallback(const ExtLearnedClauseCallback& callback);
	void setExtProbingLearnedClauseCallback(const ProbingLearnedClauseCallback& callback);

	void setCurrentRevision(int revision) {
		if (_import_manager) _import_manager->updateSolverRevision(revision);
	}

	Logger& getLogger() {return _logger;}
	
	const SolverSetup& getSolverSetup() {return _setup;}
	const SolverStatistics& getSolverStats() {
		writeStatistics(_stats);
		return _stats;
	}
	SolverStatistics& getSolverStatsRef() {
		return _stats;
	}

	void interrupt();
	void uninterrupt();
	void setTerminate();

	// Add a learned clause to the formula
	// The learned clauses might be added later or possibly never
	void addLearnedClause(const Mallob::Clause& c);
	void addLearnedClauses(BufferReader& reader, int revision) {
		if (_clause_sharing_disabled) return;
		_import_manager->setImportedRevision(revision);
		_import_manager->performImport(reader);
	}

	// Within the solver, fetch a clause that was previously added as a learned clause.
	bool fetchLearnedClause(Mallob::Clause& clauseOut, GenericClauseStore::ExportMode mode = GenericClauseStore::ANY);
	std::vector<int> fetchLearnedUnitClauses();

	std::function<void(int)> _cb_result_found;
	void setCallbackResultFound(std::function<void(int)> cb) {_cb_result_found = cb;}
	void setFoundResult() {
		_cb_result_found(_local_id);
	}

	bool _has_preprocessed_formula {false};
	std::vector<int> _preprocessed_formula;
	void setPreprocessedFormula(std::vector<int>&& vec) {
		_preprocessed_formula = std::move(vec);
		LOGGER(_logger, V4_VVER, "Set preprocessed formula: %s\n", StringUtils::getSummary(_preprocessed_formula, 20).c_str());
		_has_preprocessed_formula = true;
	}
	bool hasPreprocessedFormula() const {return _has_preprocessed_formula;}
	std::vector<int>&& extractPreprocessedFormula() {
		_has_preprocessed_formula = false;
		return std::move(_preprocessed_formula);
	}
	// void resetPreprocessedFormula() {
		// _has_preprocessed_formula = false;
		// _preprocessed_formula.clear();
	// }

	LratConnector* getLratConnector() {
		return _lrat;
	}
	OptimizingPropagator* getOptimizer() {
		return _optimizer.get();
	}

private:
	std::string _global_name;
	std::string _job_name;
	int _global_id;
	int _local_id;
	int _diversification_index;
	bool _clause_sharing_disabled = false;
	std::vector<int> _conditional_lits; // to append to each exported clause to make it global
	std::atomic_bool _terminated = false;

	SolverStatistics _stats;
	std::unique_ptr<GenericImportManager> _import_manager;

	SplitMix64Rng _rng;
};

// Returns the elapsed time (seconds) since the currently registered solver's start time.
double getTime();

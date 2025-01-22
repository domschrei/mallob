
#pragma once

#include <algorithm>                                // for max
#include <cstdint>                                  // for uint32_t
#include <cstring>                                  // for size_t
#include <list>                                     // for list
#include <memory>                                   // for unique_ptr, share...
#include <string>                                   // for string
#include <vector>                                   // for vector

#include "../data/sharing_statistics.hpp"           // for SharingStatistics
#include "app/sat/data/clause.hpp"                  // for Clause
#include "app/sat/data/clause_histogram.hpp"        // for ClauseHistogram
#include "app/sat/data/definitions.hpp"             // for ExtLearnedClauseC...
#include "app/sat/data/solver_statistics.hpp"       // for SolverStatistics
#include "app/sat/sharing/clause_id_alignment.hpp"  // for ClauseIdAlignment
#include "util/tsl/robin_hash.h"                    // for robin_hash<>::buc...
#include "util/tsl/robin_set.h"                     // for robin_set

class ClauseLogger;
class DeterministicClauseSynchronizer;
class GenericClauseFilter;
class GenericClauseStore;
class GenericExportManager;
class Logger;
class PortfolioSolverInterface;
struct Parameters;

#define CLAUSE_LEN_HIST_LENGTH 256

#define MALLOB_RESET_LBD_NEVER 0
#define MALLOB_RESET_LBD_AT_IMPORT 1
#define MALLOB_RESET_LBD_AT_EXPORT 2
#define MALLOB_RESET_LBD_AT_PRODUCE 3

struct Parameters;

class SharingManager {

protected:
	// associated solvers
	std::vector<std::shared_ptr<PortfolioSolverInterface>>& _solvers;
	
	std::vector<int> _solver_revisions;

	SolverStatistics _returned_clauses_stats;

	// clause importing / digestion
	struct DeferredClauseList {
		int revision;
		size_t numLits = 0;
		std::vector<bool> involvedSolvers;
		std::vector<int> buffer;
		std::vector<Mallob::Clause> clauses;
		std::vector<uint32_t> producersPerClause;
	};
	std::list<DeferredClauseList> _future_clauses;

	// global parameters
	const Parameters& _params;
	const Logger& _logger;
	int _job_index;

	std::unique_ptr<GenericClauseStore> _clause_store;
	std::unique_ptr<GenericClauseFilter> _clause_filter;
	std::unique_ptr<GenericExportManager> _export_buffer;
	bool _gc_pending {false};
	
	int _last_num_cls_to_import = 0;
	int _last_num_admitted_cls_to_import = 0;
	int _last_num_admitted_lits_to_import = 0;

	ClauseHistogram _hist_produced;
	ClauseHistogram _hist_returned_to_db;

	SharingStatistics _stats;
	std::vector<SolverStatistics*> _solver_stats;

	int _num_original_vars;
	int _num_original_clauses;

	int _imported_revision = -1;

	bool _observed_nonunit_lbd_of_zero = false;
	bool _observed_nonunit_lbd_of_one = false;
	bool _observed_nonunit_lbd_of_two = false;
	bool _observed_nonunit_lbd_of_length = false;
	bool _observed_nonunit_lbd_of_length_minus_one = false;

	int _internal_epoch = 0;
	tsl::robin_set<int> _digested_epochs;

	std::unique_ptr<DeterministicClauseSynchronizer> _det_sync;
	int _global_solver_id_with_result {-1};

	std::unique_ptr<ClauseIdAlignment> _id_alignment;
	bool _sharing_op_ongoing {false};

	std::unique_ptr<ClauseLogger> _clause_logger;

public:
	SharingManager(std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
			const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver,
			int jobIndex);
	~SharingManager();

	void addSharingEpoch(int epoch) {_digested_epochs.insert(epoch);}
	std::vector<int> prepareSharing(int totalLiteralLimit, int& outSuccessfulSolverId, int& outNbLits);
	std::vector<int> filterSharing(std::vector<int>& clauseBuf);
	void digestSharingWithFilter(std::vector<int>& clauseBuf, std::vector<int>* filter);
	void digestSharingWithoutFilter(std::vector<int>& clauseBuf, bool stateless);
	void returnClauses(std::vector<int>& clauseBuf);
	void digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>& clauseBuf);
	void collectGarbageInFilter();

	void setWinningSolverId(int globalId);
	bool syncDeterministicSolvingAndCheckForWinningSolver();

	SharingStatistics getStatistics();

	void setImportedRevision(int revision) {_imported_revision = std::max(_imported_revision, revision);}
	void stopClauseImport(int solverId);

	void continueClauseImport(int solverId);
	int getLastNumClausesToImport() const {return _last_num_cls_to_import;}
	int getLastNumAdmittedClausesToImport() const {return _last_num_admitted_cls_to_import;}
	int getLastNumAdmittedLitsToImport() const {return _last_num_admitted_lits_to_import;}

	int getGlobalStartOfSuccessEpoch() {
		return !_id_alignment ? 0 : _id_alignment->getGlobalStartOfSuccessEpoch();
	}
	void writeClauseEpochs(const std::string& filename) {
		if (!_id_alignment) return;
		_id_alignment->writeClauseEpochs(filename);
	}

	bool isSharingOperationOngoing() const {
		return _sharing_op_ongoing;
	}

private:

	void applyFilterToBuffer(std::vector<int>& clauseBuf, std::vector<int>* filter);

	void onProduceClause(int solverId, int solverRevision, const Mallob::Clause& clause, const std::vector<int>& condLits, bool recursiveCall = false);

	ExtLearnedClauseCallback getCallback() {
		return [this](const Mallob::Clause& c, int solverId, int solverRevision, const std::vector<int>& condLits) {
			onProduceClause(solverId, solverRevision, c, condLits);
		};
	};

	void tryReinsertDeferredClauses(int solverId, std::list<Mallob::Clause>& clauses, SolverStatistics* stats);
	void digestDeferredFutureClauses();

	void importClausesToSolver(int solverId, const std::vector<Mallob::Clause>& clauses, const std::vector<uint32_t>& producersPerClause);

};


#pragma once

#include <cstring>
#include <memory>
#include <list>

#include "app/sat/sharing/clause_id_alignment.hpp"
#include "app/sat/sharing/generic_export_manager.hpp"
#include "../solvers/portfolio_solver_interface.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "util/params.hpp"
#include "../data/sharing_statistics.hpp"
#include "util/tsl/robin_map.h"
#include "util/tsl/robin_set.h"
#include "buffer/deterministic_clause_synchronizer.hpp"

#define CLAUSE_LEN_HIST_LENGTH 256

#define MALLOB_RESET_LBD_NEVER 0
#define MALLOB_RESET_LBD_AT_IMPORT 1
#define MALLOB_RESET_LBD_AT_EXPORT 2
#define MALLOB_RESET_LBD_AT_PRODUCE 3

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
	int _allocated_sharing_buffer_size {-1};

	ClauseHistogram _hist_produced;
	ClauseHistogram _hist_returned_to_db;

	SharingStatistics _stats;
	std::vector<SolverStatistics*> _solver_stats;

	int _num_original_clauses;

	int _current_revision = -1;

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

public:
	SharingManager(std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
			const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver,
			int jobIndex);
	~SharingManager();

	void setAllocatedSharingBufferSize(int allocatedSize) {_allocated_sharing_buffer_size = allocatedSize;}
	void addSharingEpoch(int epoch) {_digested_epochs.insert(epoch);}
    int prepareSharing(int* begin, int totalLiteralLimit, int& successfulSolverId);
	int filterSharing(int* begin, int buflen, int* filterOut);
	void digestSharingWithFilter(int* begin, int buflen, const int* filter);
    void digestSharingWithoutFilter(int* begin, int buflen);
	void returnClauses(int* begin, int buflen);
	void digestHistoricClauses(int epochBegin, int epochEnd, int* begin, int buflen);
	void collectGarbageInFilter();

	void setWinningSolverId(int globalId);
	bool syncDeterministicSolvingAndCheckForWinningSolver();

	SharingStatistics getStatistics();

	void setRevision(int revision) {_current_revision = revision;}
	void stopClauseImport(int solverId);

	void continueClauseImport(int solverId);
	int getLastNumClausesToImport() const {return _last_num_cls_to_import;}
	int getLastNumAdmittedClausesToImport() const {return _last_num_admitted_cls_to_import;}

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

	void applyFilterToBuffer(int* begin, int& buflen, const int* filter);

	void onProduceClause(int solverId, int solverRevision, const Mallob::Clause& clause, int condVarOrZero, bool recursiveCall = false);

	ExtLearnedClauseCallback getCallback() {
		return [this](const Mallob::Clause& c, int solverId, int solverRevision, int condVarOrZero) {
			onProduceClause(solverId, solverRevision, c, condVarOrZero);
		};
	};

	void tryReinsertDeferredClauses(int solverId, std::list<Mallob::Clause>& clauses, SolverStatistics* stats);
	void digestDeferredFutureClauses();

	void importClausesToSolver(int solverId, const std::vector<Mallob::Clause>& clauses, const std::vector<uint32_t>& producersPerClause);

};

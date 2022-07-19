
#pragma once

#include <cstring>
#include <memory>
#include <list>

#include "buffer/adaptive_clause_database.hpp"
#include "../solvers/portfolio_solver_interface.hpp"
#include "util/params.hpp"
#include "filter/produced_clause_filter.hpp"
#include "export_buffer.hpp"
#include "../data/sharing_statistics.hpp"

#define CLAUSE_LEN_HIST_LENGTH 256

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
	size_t _max_deferred_lits_per_solver;
	
	// global parameters
	const Parameters& _params;
	const Logger& _logger;
	int _job_index;

	ProducedClauseFilter _filter;
	AdaptiveClauseDatabase _cdb;
	ExportBuffer _export_buffer;
	
	int _last_num_cls_to_import = 0;
	int _last_num_admitted_cls_to_import = 0;

	ClauseHistogram _hist_produced;
	ClauseHistogram _hist_returned_to_db;

	SharingStatistics _stats;
	std::vector<SolverStatistics*> _solver_stats;

	std::vector<std::atomic_ulong*> _last_exported_clause_id; 
	typedef std::vector<unsigned long> EpochIdList;
	std::vector<EpochIdList> _min_epoch_ids_per_solver;
	std::vector<EpochIdList> _id_offsets_per_solver;
	EpochIdList _global_epoch_ids;

	int _current_revision = -1;

	bool _observed_nonunit_lbd_of_zero = false;
	bool _observed_nonunit_lbd_of_one = false;
	bool _observed_nonunit_lbd_of_two = false;
	bool _observed_nonunit_lbd_of_length = false;
	bool _observed_nonunit_lbd_of_length_minus_one = false;

	int _internal_epoch = 0;

public:
	SharingManager(std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
			const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver,
			int jobIndex);
	~SharingManager();

    int prepareSharing(int* begin, int totalLiteralLimit);
	int filterSharing(int* begin, int buflen, int* filterOut);
	void digestSharingWithFilter(int* begin, int buflen, const int* filter);
    void digestSharingWithoutFilter(int* begin, int buflen);
	void returnClauses(int* begin, int buflen);

	SharingStatistics getStatistics();

	void setRevision(int revision) {_current_revision = revision;}
	void stopClauseImport(int solverId);

	void continueClauseImport(int solverId);
	int getLastNumClausesToImport() const {return _last_num_cls_to_import;}
	int getLastNumAdmittedClausesToImport() const {return _last_num_admitted_cls_to_import;}

	void writeClauseEpochs(const std::string& filename);

private:

	int getProducingLocalSolverIndex(unsigned long clauseId) {
		return clauseId % _solvers.size();
	}
	void alignClauseId(int* clauseData) {
		unsigned long clauseId = metadata::readUnsignedLong(clauseData);
		int localSolverId = getProducingLocalSolverIndex(clauseId);
		// take the offset that belongs to the clause's epoch!
		auto offset = _id_offsets_per_solver[localSolverId][getEpochOfUnalignedSelfClause(clauseId)];
		unsigned long alignedClauseId = clauseId + offset;

		assert(getEpochOfAlignedSelfClause(alignedClauseId) == getEpochOfUnalignedSelfClause(clauseId));
		assert(getProducingLocalSolverIndex(alignedClauseId) == getProducingLocalSolverIndex(clauseId));

		metadata::writeUnsignedLong(alignedClauseId, clauseData);		
	}
	void unalignClauseId(int* clauseData) {
		unsigned long clauseId = metadata::readUnsignedLong(clauseData);
		int localSolverId = getProducingLocalSolverIndex(clauseId);
		auto offset = _id_offsets_per_solver[localSolverId][getEpochOfAlignedSelfClause(clauseId)];
		unsigned long unalignedClauseId = clauseId - offset;

		assert(getEpochOfAlignedSelfClause(clauseId) == getEpochOfUnalignedSelfClause(unalignedClauseId));
		assert(getProducingLocalSolverIndex(clauseId) == getProducingLocalSolverIndex(unalignedClauseId));

		metadata::writeUnsignedLong(unalignedClauseId, clauseData);
	}
	int getEpochOfUnalignedSelfClause(unsigned long id);
	int getEpochOfAlignedSelfClause(unsigned long id);

	void onProduceClause(int solverId, int solverRevision, const Clause& clause, int condVarOrZero);

	ExtLearnedClauseCallback getCallback() {
		return [this](const Clause& c, int solverId, int solverRevision, int condVarOrZero) {
			onProduceClause(solverId, solverRevision, c, condVarOrZero);
		};
	};

	void tryReinsertDeferredClauses(int solverId, std::list<Clause>& clauses, SolverStatistics* stats);
	void digestDeferredFutureClauses();

	void importClausesToSolver(int solverId, const std::vector<Clause>& clauses, const std::vector<uint32_t>& producersPerClause);

};

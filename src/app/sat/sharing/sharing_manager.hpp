
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
#include "buffer/deterministic_clause_synchronizer.hpp"

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

	int _num_original_clauses;
	int _max_num_threads;
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

	std::unique_ptr<DeterministicClauseSynchronizer> _det_sync;
	int _global_solver_id_with_result {-1};

public:
	SharingManager(std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers,
			const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver,
			int jobIndex);
	~SharingManager();

    int prepareSharing(int* begin, int totalLiteralLimit, int& successfulSolverId);
	int filterSharing(int* begin, int buflen, int* filterOut);
	void digestSharingWithFilter(int* begin, int buflen, const int* filter);
    void digestSharingWithoutFilter(int* begin, int buflen);
	void returnClauses(int* begin, int buflen);

	void setWinningSolverId(int globalId);
	bool syncDeterministicSolvingAndCheckForWinningSolver();

	SharingStatistics getStatistics();

	void setRevision(int revision) {_current_revision = revision;}
	void stopClauseImport(int solverId);

	void continueClauseImport(int solverId);
	int getLastNumClausesToImport() const {return _last_num_cls_to_import;}
	int getLastNumAdmittedClausesToImport() const {return _last_num_admitted_cls_to_import;}

	void writeClauseEpochs(/*const std::string& proofDir, int firstGlobalId, */
		const std::string& outputFilename);
		
	unsigned long getGlobalStartOfSuccessEpoch() {
		return _global_epoch_ids.empty() ? 0 : _global_epoch_ids.back();
	}

private:

	bool isLocallyProducedClause(unsigned long clauseId) {
		auto globalId = (clauseId-_num_original_clauses-1) % _solvers[0]->getSolverSetup().maxNumSolvers;
		for (auto& solver : _solvers) if (solver->getGlobalId() == globalId) return true;
		return false;
	}

	int getProducingLocalSolverIndex(unsigned long clauseId) {
		return (clauseId-_num_original_clauses-1) % _max_num_threads;
	}
	int getProducingInstanceId(unsigned long clauseId) {
		return (clauseId-_num_original_clauses-1) % _solvers[0]->getSolverSetup().maxNumSolvers;
	}
	void alignClauseId(int* clauseData) {

		// Only align clause IDs if distributed proof assembly is done
		if (!_params.distributedProofAssembly()) return;

		unsigned long clauseId = ClauseMetadata::readUnsignedLong(clauseData);
		int localSolverId = getProducingLocalSolverIndex(clauseId);

		// take the offset that belongs to the clause's epoch!
		int epoch = getEpochOfUnalignedSelfClause(clauseId);
		assert(epoch >= 0 && epoch < _id_offsets_per_solver[localSolverId].size() 
			|| log_return_false("Invalid epoch %i found for clause ID %lu\n", epoch, clauseId));
		auto offset = _id_offsets_per_solver[localSolverId][epoch];
		unsigned long alignedClauseId = clauseId + offset;

		LOG(V5_DEBG, "ALIGN EPOCH=%i %lu => %lu\n", epoch, clauseId, alignedClauseId);

		assert(getEpochOfAlignedSelfClause(alignedClauseId) == getEpochOfUnalignedSelfClause(clauseId));
		assert(getProducingLocalSolverIndex(alignedClauseId) == getProducingLocalSolverIndex(clauseId));

		ClauseMetadata::writeUnsignedLong(alignedClauseId, clauseData);
	}
	void unalignClauseId(int* clauseData) {

		// Only align clause IDs if distributed proof assembly is done
		if (!_params.distributedProofAssembly()) return;

		unsigned long clauseId = ClauseMetadata::readUnsignedLong(clauseData);
		int localSolverId = getProducingLocalSolverIndex(clauseId);

		int epoch = getEpochOfAlignedSelfClause(clauseId);
		assert(epoch >= 0 && epoch < _id_offsets_per_solver[localSolverId].size() 
			|| log_return_false("Invalid epoch %i found for clause ID %lu\n", epoch, clauseId));
		auto offset = _id_offsets_per_solver[localSolverId][epoch];
		unsigned long unalignedClauseId = clauseId - offset;

		LOG(V5_DEBG, "UNALIGN EPOCH=%i %lu => %lu\n", epoch, clauseId, unalignedClauseId);

		assert(getEpochOfAlignedSelfClause(clauseId) == getEpochOfUnalignedSelfClause(unalignedClauseId) 
			|| log_return_false("[ERROR] epoch of aligned clause %lu: %i; epoch of unaligned clause %lu: %i\n",
			clauseId, getEpochOfAlignedSelfClause(clauseId), unalignedClauseId, getEpochOfUnalignedSelfClause(unalignedClauseId)));
		assert(getProducingLocalSolverIndex(clauseId) == getProducingLocalSolverIndex(unalignedClauseId));

		ClauseMetadata::writeUnsignedLong(unalignedClauseId, clauseData);
	}
	int getEpochOfUnalignedSelfClause(unsigned long id);
	int getEpochOfAlignedSelfClause(unsigned long id);

	void onProduceClause(int solverId, int solverRevision, const Clause& clause, int condVarOrZero, bool recursiveCall = false);

	ExtLearnedClauseCallback getCallback() {
		return [this](const Clause& c, int solverId, int solverRevision, int condVarOrZero) {
			onProduceClause(solverId, solverRevision, c, condVarOrZero);
		};
	};

	void tryReinsertDeferredClauses(int solverId, std::list<Clause>& clauses, SolverStatistics* stats);
	void digestDeferredFutureClauses();

	void importClausesToSolver(int solverId, const std::vector<Clause>& clauses, const std::vector<uint32_t>& producersPerClause);

};

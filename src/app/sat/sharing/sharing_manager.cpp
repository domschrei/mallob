/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include <signal.h>
#include <algorithm>
#include <fstream>
#include <cstdio>
#include <unistd.h>

#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/sharing/store/static_clause_store_by_lbd.hpp"
#include "sharing_manager.hpp"
#include "app/sat/sharing/store/static_clause_store.hpp"
#include "app/sat/sharing/store/adaptive_clause_store.hpp"
#include "app/sat/sharing/filter/noop_clause_filter.hpp"
#include "app/sat/sharing/filter/bloom_clause_filter.hpp"
#include "app/sat/sharing/filter/exact_clause_filter.hpp"
#include "app/sat/sharing/simple_export_manager.hpp"
#include "app/sat/sharing/backlog_export_manager.hpp"
#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/data/solver_statistics.hpp"
#include "app/sat/sharing/buffer/deterministic_clause_synchronizer.hpp"
#include "app/sat/sharing/clause_id_alignment.hpp"
#include "app/sat/sharing/filter/importing_solver.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "util/random.hpp"
#include "filter/in_place_clause_filtering.hpp"

SharingManager::SharingManager(
		std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver, int jobIndex)
	: _solvers(solvers), _params(params), _logger(logger), _job_index(jobIndex),
	_clause_store([&]() -> GenericClauseStore* {
		bool resetLbdAtExport = _params.resetLbd() == MALLOB_RESET_LBD_AT_EXPORT;
		switch(_params.clauseStoreMode()) {
		case MALLOB_CLAUSE_STORE_STATIC_BY_LENGTH:
			return new StaticClauseStore(_params.strictClauseLengthLimit(),
				resetLbdAtExport);
		case MALLOB_CLAUSE_STORE_STATIC_BY_LBD:
			return new StaticClauseStoreByLbd(_params.strictClauseLengthLimit(),
				resetLbdAtExport);
		case MALLOB_CLAUSE_STORE_ADAPTIVE:
		default:
			AdaptiveClauseStore::Setup setup;
			setup.maxClauseLength = _params.strictClauseLengthLimit();
			setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
			setup.numLiterals = _params.clauseBufferBaseSize()*_params.numChunksForExport();
			setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
			setup.resetLbdAtExport = resetLbdAtExport;
			return new AdaptiveClauseStore(setup);
		}
	}()),
	_clause_filter([&]() -> GenericClauseFilter* {
		switch (_params.clauseFilterMode()) {
		case MALLOB_CLAUSE_FILTER_NONE:
			return new NoopClauseFilter(*_clause_store);
		case MALLOB_CLAUSE_FILTER_BLOOM:
			return new BloomClauseFilter(*_clause_store, _solvers.size(), _params.strictClauseLengthLimit());
		case MALLOB_CLAUSE_FILTER_EXACT:
		case MALLOB_CLAUSE_FILTER_EXACT_DISTRIBUTED:
		default:
			return new ExactClauseFilter(*_clause_store, _params.clauseFilterClearInterval(), _params.strictClauseLengthLimit());
		}
	}()),
	_export_buffer([&]() -> GenericExportManager* {
		if (_params.backlogExportManager()) {
			return new BacklogExportManager(*_clause_store.get(), *_clause_filter.get(),
				_solvers, _solver_stats, params.strictClauseLengthLimit());
		} else {
			return new SimpleExportManager(*_clause_store.get(), *_clause_filter.get(),
				_solvers, _solver_stats, params.strictClauseLengthLimit());
		}
	}()),
	_hist_produced(params.strictClauseLengthLimit()), 
	_hist_returned_to_db(params.strictClauseLengthLimit()) {

	_stats.histProduced = &_hist_produced;
	_stats.histFailedFilter = &_export_buffer->getFailedFilterHistogram();
	_stats.histAdmittedToDb = &_export_buffer->getAdmittedHistogram();
	_stats.histDroppedBeforeDb = &_export_buffer->getDroppedHistogram();
	_stats.histDeletedInSlots = &_clause_store->getDeletedClausesHistogram();
	_stats.histReturnedToDb = &_hist_returned_to_db;

	auto callback = getCallback();
	
	if (solvers.empty()) return;
	_num_original_clauses = solvers[0]->getSolverSetup().numOriginalClauses;
	int maxNumGlobalSolvers = solvers[0]->getSolverSetup().maxNumSolvers;

	if (ClauseMetadata::enabled() && _params.distributedProofAssembly()) {
		_id_alignment.reset(new ClauseIdAlignment(_logger, _solvers, _num_original_clauses, _params.numThreadsPerProcess()));
	}

	for (size_t i = 0; i < _solvers.size(); i++) {
		_solvers[i]->setExtLearnedClauseCallback(callback);
		_solvers[i]->setCallbackResultFound([&](int localId) {
			if (_det_sync) _det_sync->notifySolverDone(localId);
		});
		_solver_revisions.push_back(_solvers[i]->getSolverSetup().solverRevision);
		_solver_stats.push_back(&_solvers[i]->getSolverStatsRef());
	}

	if (_params.deterministicSolving()) {
		_det_sync.reset(new DeterministicClauseSynchronizer(_solvers, _num_original_clauses, [&](auto call) {
			onProduceClause(call.solverId, call.solverRevision, call.clause, call.condVarOrZero, true);
		}));
	}
}

void SharingManager::onProduceClause(int solverId, int solverRevision, const Clause& clause, int condVarOrZero, bool recursiveCall) {

	if (!recursiveCall && _det_sync) {
		// Deterministic solving!
		_det_sync->insertBlocking(solverId, solverRevision, clause, condVarOrZero);
		return;
	}

	if (_solver_revisions[solverId] != solverRevision) return;

	if (_params.crashMonkeyProbability() > 0) {
		if (Random::rand() < _params.crashMonkeyProbability()) {
			// Crash!
			LOGGER(_logger, V3_VERB, "Simulating a crash!\n");
			raise(SIGSEGV); // causes crash
		}
	}

	if (_id_alignment) _id_alignment->onProduceClause(clause, solverId);

	auto clauseBegin = clause.begin;
	auto clauseSize = clause.size;

	// If necessary, apply a transformation to the clause:
	// Add the supplied conditional variable in negated form to the clause.
	// This effectively renders the found conflict relative to the assumptions
	// which were added not as assumptions but as permanent unit clauses.
	std::vector<int>* tldClauseVec = nullptr;
	if (condVarOrZero != 0) {
		tldClauseVec = new std::vector<int>(clause.size+1);
		for (int i = 0; i < clause.size; i++) tldClauseVec->at(i) = clause.begin[i];
		tldClauseVec->at(clause.size) = -condVarOrZero;
		clauseBegin = tldClauseVec->data();
		clauseSize++;
    }

    // Check maximum size of clause
    if (clauseSize > _params.strictClauseLengthLimit()) {
        if (tldClauseVec) delete tldClauseVec;
        return;
    }

	if (clauseSize == 1 && clause.lbd != 1) {
		_logger.log(V1_WARN, "Observed unit LBD of %i\n", clause.lbd);
	}
	if (clauseSize-ClauseMetadata::numBytes() > 1) {
		_observed_nonunit_lbd_of_zero |= clause.lbd == 0;
		_observed_nonunit_lbd_of_one |= clause.lbd == 1;
		_observed_nonunit_lbd_of_two |= clause.lbd == 2;
		_observed_nonunit_lbd_of_length_minus_one |= clause.lbd == clause.size-ClauseMetadata::numBytes()-1;
		_observed_nonunit_lbd_of_length |= clause.lbd == clause.size-ClauseMetadata::numBytes();
	}

	if (clauseSize == 1) assert(clause.lbd == 1);
	else {
		assert(clause.lbd >= 1 || LOG_RETURN_FALSE("[ERROR] len=%i lbd=%i!\n", clause.size, clause.lbd));
		assert(clause.lbd <= clause.size || LOG_RETURN_FALSE("[ERROR] len=%i lbd=%i!\n", clause.size, clause.lbd));
	}
	int clauseLbd = clauseSize == 1 ? 1 : std::max(2, clause.lbd + (condVarOrZero == 0 ? 0 : 1));

	if (_params.resetLbd() == MALLOB_RESET_LBD_AT_PRODUCE)
		clauseLbd = clauseSize;

	// Add clause length to statistics
	_hist_produced.increment(clauseSize);
	auto& solverStats = _solver_stats[solverId];
	if (solverStats) {
		solverStats->producedClauses++;
		solverStats->histProduced->increment(clause.size);
	}

	// Sort literals in clause
	std::sort(clauseBegin+ClauseMetadata::numBytes(), clauseBegin+clauseSize);

	_export_buffer->produce(clauseBegin, clauseSize, clauseLbd, solverId, _internal_epoch);
	//log(V6_DEBGV, "%i : PRODUCED %s\n", solverId, tldClause.toStr().c_str());

	if (tldClauseVec) delete tldClauseVec;
}

int SharingManager::prepareSharing(int* begin, int totalLiteralLimit, int& successfulSolverId) {

	if (_det_sync) {
		if (!_det_sync->areAllSolversSyncReady()) return -1;
		successfulSolverId = _det_sync->waitUntilSyncReadyAndReturnSolverIdWithResult();
		LOGGER(_logger, V4_VVER, "All solvers synced\n");
		if (successfulSolverId >= 0) {
			LOGGER(_logger, V4_VVER, "Emit successful solver ID %i\n", successfulSolverId);
		}
	}

	_sharing_op_ongoing = true;

	// Flushing the priority clause buffer results in owning locks
	// for an extended period, which may block solver threads.
	// Lock all filters such that solvers write to backlogs instead.
	float time = Timer::elapsedSeconds();
	_clause_filter->acquireAllLocks();
	time = Timer::elapsedSeconds() - time;
	LOGGER(_logger, V4_VVER, "acquired all clause locks after %.6fs\n", time);

	int numExportedClauses = 0;
	auto buffer = _clause_store->exportBuffer(totalLiteralLimit, numExportedClauses, 
			GenericClauseStore::ANY, /*sortClauses=*/true, [&](int* data) {

		// Shift clause ID from a local solver according to the solver's offset
		if (_id_alignment) _id_alignment->alignClauseId(data);
	});

	_clause_filter->releaseAllLocks();

	if (_allocated_sharing_buffer_size >= 0 && buffer.size() > _allocated_sharing_buffer_size) {
		LOGGER(_logger, V1_WARN, "[WARN] prepared buffer len=%i exceeds allocated size %i! Truncating ...\n",
			(int)buffer.size(), _allocated_sharing_buffer_size);
		// Truncating should be fine, even if a clause is cut in half,
		// since BufferReader implementation always checks for bounds.
		buffer.resize(_allocated_sharing_buffer_size);
	}

	//assert(buffer.size() <= maxSize);
	memcpy(begin, buffer.data(), buffer.size()*sizeof(int));

	LOGGER(_logger, V5_DEBG, "prepared %i clauses, size %i (%i in DB, limit %i)\n", numExportedClauses, buffer.size(), 
		_pcb.getCurrentlyUsedLiterals(), totalLiteralLimit);
	_stats.exportedClauses += numExportedClauses;
	_internal_epoch++;
	_clause_filter->updateEpoch(_internal_epoch);

	return buffer.size();
}

void SharingManager::returnClauses(int* begin, int buflen) {

	auto reader = _clause_store->getBufferReader(begin, buflen);

	// No clause ID alignments: Can just reinsert all clauses, no questions asked
	if (!_id_alignment) {
		_clause_store->addClauses(reader, &_hist_returned_to_db);
		return;
	}

	auto c = reader.getNextIncomingClause();
	while (c.begin != nullptr) {

		// For certified UNSAT we need to drop returned clauses which do not
		// originate from this solver, since we can not un-align them to
		// correctly insert them into the database.
		if (_id_alignment->isLocallyProducedClause(ClauseMetadata::readUnsignedLong(c.begin))) {

			// Returned clauses would be aligned *again* when re-exported.
			// => subtract the offsets again here ...
			_id_alignment->unalignClauseId(c.begin);

			bool success = _clause_store->addClause(c);
			if (success) _hist_returned_to_db.increment(c.size);
		}

		c = reader.getNextIncomingClause();
	}
}

int SharingManager::filterSharing(int* begin, int buflen, int* filterOut) {

	auto reader = _clause_store->getBufferReader(begin, buflen);
	
	constexpr auto bitsPerElem = 8*sizeof(int);
	int shift = bitsPerElem;
	auto clause = reader.getNextIncomingClause();
	int filterPos = -1 + ClauseMetadata::numBytes();
	int nbFiltered = 0;
	int nbTotal = 0;

	if (_id_alignment) {
		_id_alignment->contributeFirstClauseIdOfEpoch(filterOut);
	}

	int filterSizeBeingLocked = -1;
	while (clause.begin != nullptr) {
		++nbTotal;

		if (filterSizeBeingLocked != clause.size) {
			if (filterSizeBeingLocked != -1) _clause_filter->releaseLock(filterSizeBeingLocked);
			filterSizeBeingLocked = clause.size;
			_clause_filter->acquireLock(filterSizeBeingLocked);
		}

		if (shift == bitsPerElem) {
			++filterPos;
			filterOut[filterPos] = 0;
			shift = 0;
		}
		
		if (!_clause_filter->admitSharing(clause, _internal_epoch)) {
			// filtered!
			auto bitFiltered = 1 << shift;
			filterOut[filterPos] |= bitFiltered;
			++nbFiltered;
		}
		
		++shift;
		clause = reader.getNextIncomingClause();
	}
	if (filterSizeBeingLocked != -1) _clause_filter->releaseLock(filterSizeBeingLocked);

	_logger.log(V4_VVER, "filtered %i/%i\n", nbFiltered, nbTotal);
	return filterPos+1;
}

void SharingManager::digestSharingWithFilter(int* begin, int buflen, const int* filter) {
	int verb = _job_index == 0 ? V3_VERB : V5_DEBG;

	float time = Timer::elapsedSeconds();
	ClauseHistogram hist(_params.strictClauseLengthLimit());

	_logger.log(verb, "digesting len=%ld\n", buflen);

	std::vector<ImportingSolver> importingSolvers;
	for (size_t i = 0; i < _solvers.size(); i++) {
		auto& solver = _solvers[i];
		if (!solver || !_solver_stats[i]) continue; // solver was cleaned up
		if (!solver->isClauseSharingEnabled()) continue;
		if (solver->getCurrentRevision() != _current_revision) continue;
		importingSolvers.emplace_back(solver.get(), _solver_stats[i], _id_alignment.get());
	}

	_last_num_cls_to_import = 0;
	_last_num_admitted_cls_to_import = 0;

	if (importingSolvers.empty()) {
		_logger.log(verb, "no local solvers accepting clauses - ignoring sharing\n");
		_gc_pending = true;
		_sharing_op_ongoing = false;
		return;
	}

	// Apply provided global filter to buffer (in-place operation)
	applyFilterToBuffer(begin, buflen, filter);

	auto reader = _clause_store->getBufferReader(begin, buflen);

	_logger.log(verb+2, "DG import\n");

	// For each incoming clause (which was not filtered out)
	int filterSizeBeingLocked = -1;
	auto clause = reader.getNextIncomingClause();
	while (clause.begin != nullptr) {

		if (filterSizeBeingLocked != clause.size) {
			if (filterSizeBeingLocked != -1) _clause_filter->releaseLock(filterSizeBeingLocked);
			filterSizeBeingLocked = clause.size;
			_clause_filter->acquireLock(filterSizeBeingLocked);
		}

		hist.increment(clause.size);
		// bitset of producing solvers
		auto producers = _clause_filter->confirmSharingAndGetProducers(clause, _internal_epoch);

		// Decide for each solver whether it should receive the clause
		for (size_t i = 0; i < importingSolvers.size(); i++) {
			importingSolvers[i].appendCandidate(clause, producers);
		}

		clause = reader.getNextIncomingClause();
	}
	if (filterSizeBeingLocked != -1) _clause_filter->releaseLock(filterSizeBeingLocked);
	
	for (auto& slv : importingSolvers) {
		BufferReader reader = _clause_store->getBufferReader(begin, buflen);
		reader.setFilterBitset(slv.filter);
		slv.solver->addLearnedClauses(reader);
	}
	
	// Process-wide stats
	time = Timer::elapsedSeconds() - time;
	_logger.log(verb, "sharing time:%.4f adm:%i/%i %s\n", time, 
		_last_num_admitted_cls_to_import, _last_num_cls_to_import, hist.getReport().c_str());

	// Signal next garbage collection
	_gc_pending = true;
	_sharing_op_ongoing = false;
}

void SharingManager::applyFilterToBuffer(int* begin, int& buflen, const int* filter) {

	int verb = _job_index == 0 ? V3_VERB : V5_DEBG;
	if (ClauseMetadata::enabled()) assert(filter != nullptr);
	if (filter == nullptr) return;

	_logger.log(verb+2, "DG apply global filter\n");
	const int bitsPerElem = sizeof(int)*8;
	int shift = bitsPerElem;
	int filterPos = -1 + ClauseMetadata::numBytes();

	if (_id_alignment) {
		_id_alignment->beginNextEpoch(filter);
	}

	InPlaceClauseFiltering filtering(_params, begin, buflen, filter, buflen);
	buflen = filtering.applyAndGetNewSize();
	_last_num_cls_to_import += filtering.getNumClauses();
	_last_num_admitted_cls_to_import += filtering.getNumAdmittedClauses();
}

void SharingManager::digestSharingWithoutFilter(int* begin, int buflen) {
	digestSharingWithFilter(begin, buflen, nullptr);
}

void SharingManager::digestHistoricClauses(int epochBegin, int epochEnd, int *begin, int buflen) {
	// decide whether to perform the import
	int numUnknown = 0;
	for (int e = epochBegin; e < epochEnd; e++) {
		if (!_digested_epochs.count(e)) numUnknown++;
	}
	if (2*numUnknown >= epochEnd-epochBegin) {
		// More than half of the historic epochs are missing: do import.
		_logger.log(V2_INFO, "Import historic cls [%i,%i) (missing %i/%i)\n", 
			epochBegin, epochEnd, numUnknown, epochEnd-epochBegin);
		digestSharingWithoutFilter(begin, buflen);
		for (int e = epochBegin; e < epochEnd; e++) addSharingEpoch(e);
	}
}

void SharingManager::collectGarbageInFilter() {
	if (!_gc_pending) return;
	_clause_filter->collectGarbage(_logger);
	_gc_pending = false;
}

void SharingManager::setWinningSolverId(int globalId) {
	_global_solver_id_with_result = globalId;
	_logger.log(V5_DEBG, "S%i is global winner\n", globalId);
}

bool SharingManager::syncDeterministicSolvingAndCheckForWinningSolver() {
	if (!_det_sync) return false;

	// Deterministic solving: resume solvers
	return _det_sync->syncAndCheckForLocalWinner(_global_solver_id_with_result);
}

SharingStatistics SharingManager::getStatistics() {

	_logger.log(V2_INFO, "Observed non-unit LBDs: 0:%i 1:%i 2:%i len-1:%i len:%i\n", 
		_observed_nonunit_lbd_of_zero, 
		_observed_nonunit_lbd_of_one, 
		_observed_nonunit_lbd_of_two, 
		_observed_nonunit_lbd_of_length_minus_one, 
		_observed_nonunit_lbd_of_length);
	return _stats;
}

void SharingManager::stopClauseImport(int solverId) {
	assert(solverId >= 0 && solverId < _solvers.size());
	_solver_revisions[solverId] = -1;
	_solver_stats[solverId] = nullptr;
}

void SharingManager::continueClauseImport(int solverId) {
	assert(solverId >= 0 && solverId < _solvers.size());
	_solver_revisions[solverId] = _solvers[solverId]->getSolverSetup().solverRevision;
	_solvers[solverId]->setExtLearnedClauseCallback(getCallback());
	_solver_stats[solverId] = &_solvers[solverId]->getSolverStatsRef();
}

SharingManager::~SharingManager() {}


#include <climits>
#include <signal.h>
#include <assert.h>
#include <ext/alloc_traits.h>
#include <algorithm>
#include <utility>

#include "app/sat/data/model_string_compressor.hpp"
#include "app/sat/sharing/clause_logger.hpp"
#include "app/sat/sharing/filter/clause_buffer_lbd_scrambler.hpp"
#include "app/sat/sharing/filter/filter_vector_builder.hpp"
#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "app/sat/sharing/store/static_clause_store_by_lbd.hpp"
#include "app/sat/sharing/store/static_clause_store_mixed_lbd.hpp"
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
#include "util/logger.hpp"
#include "util/string_utils.hpp"
#include "util/sys/timer.hpp"
#include "util/random.hpp"
#include "filter/in_place_clause_filtering.hpp"
#include "app/sat/data/sharing_statistics.hpp"
#include "app/sat/execution/solver_setup.hpp"
#include "app/sat/sharing/buffer/buffer_reader.hpp"
#include "app/sat/sharing/generic_export_manager.hpp"
#include "robin_set.h"
#include "util/option.hpp"
#include "util/params.hpp"

SharingManager::SharingManager(
		std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver, int jobIndex)
	: _solvers(solvers), _params(params), _logger(logger), _job_index(jobIndex),
	_clause_store([&]() -> GenericClauseStore* {
		bool resetLbdAtExport = _params.resetLbd() == MALLOB_RESET_LBD_AT_EXPORT;
		int staticBucketSize = (2*_params.clauseBufferBaseSize())/3;
		switch(_params.clauseStoreMode()) {
		case MALLOB_CLAUSE_STORE_STATIC_BY_LENGTH_MIXED_LBD:
			return new StaticClauseStoreMixedLbd(_params.strictClauseLengthLimit()+ClauseMetadata::numInts(),
				resetLbdAtExport, staticBucketSize);
		case MALLOB_CLAUSE_STORE_STATIC_BY_LENGTH:
			return new StaticClauseStore<true>(_params,
				resetLbdAtExport, staticBucketSize, false, 0);
		case MALLOB_CLAUSE_STORE_STATIC_BY_LBD:
			return new StaticClauseStoreByLbd(_params.strictClauseLengthLimit()+ClauseMetadata::numInts(),
				resetLbdAtExport, staticBucketSize);
		case MALLOB_CLAUSE_STORE_ADAPTIVE_SIMPLE:
			return new StaticClauseStore<true>(_params,
				resetLbdAtExport, 256, true,
				_params.clauseBufferBaseSize()*_params.numExportChunks());
		case MALLOB_CLAUSE_STORE_ADAPTIVE:
		default:
			AdaptiveClauseStore::Setup setup;
			setup.maxEffectiveClauseLength = _params.strictClauseLengthLimit()+ClauseMetadata::numInts();
			setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
			setup.numLiterals = _params.clauseBufferBaseSize()*_params.numExportChunks();
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
			return new BloomClauseFilter(*_clause_store, _solvers.size(),
				_params.strictClauseLengthLimit()+ClauseMetadata::numInts(),
				_params.backlogExportManager());
		case MALLOB_CLAUSE_FILTER_EXACT:
		case MALLOB_CLAUSE_FILTER_EXACT_DISTRIBUTED:
		default:
			return new ExactClauseFilter(*_clause_store, _params.clauseFilterClearInterval(), _params.strictClauseLengthLimit()+ClauseMetadata::numInts());
		}
	}()),
	_export_buffer([&]() -> GenericExportManager* {
		if (_params.backlogExportManager()) {
			return new BacklogExportManager(*_clause_store.get(), *_clause_filter.get(),
				_solvers, _solver_stats, params.strictClauseLengthLimit()+ClauseMetadata::numInts());
		} else {
			return new SimpleExportManager(*_clause_store.get(), *_clause_filter.get(),
				_solvers, _solver_stats, params.strictClauseLengthLimit()+ClauseMetadata::numInts());
		}
	}()),
	_hist_produced(params.strictClauseLengthLimit()+ClauseMetadata::numInts()), 
	_hist_returned_to_db(params.strictClauseLengthLimit()+ClauseMetadata::numInts()) {

	_stats.histProduced = &_hist_produced;
	_stats.histFailedFilter = &_export_buffer->getFailedFilterHistogram();
	_stats.histAdmittedToDb = &_export_buffer->getAdmittedHistogram();
	_stats.histDroppedBeforeDb = &_export_buffer->getDroppedHistogram();
	_stats.histDeletedInSlots = &_clause_store->getDeletedClausesHistogram();
	_stats.histReturnedToDb = &_hist_returned_to_db;

	auto callback = getCallback();
	
	if (solvers.empty()) return;
	_num_original_vars = solvers[0]->getSolverSetup().numVars;
	_num_original_clauses = solvers[0]->getSolverSetup().numOriginalClauses;
	int maxNumGlobalSolvers = solvers[0]->getSolverSetup().maxNumSolvers;

	if (ClauseMetadata::enabled() && _params.proofOutputFile.isSet() && _params.distributedProofAssembly()) {
		_id_alignment.reset(new ClauseIdAlignment(_logger, _solvers, _num_original_clauses, _params.numThreadsPerProcess()));
	}

	for (size_t i = 0; i < _solvers.size(); i++) {
		_solvers[i]->setExtLearnedClauseCallback(callback);
		_solvers[i]->setExtProbingLearnedClauseCallback([&](int effectiveClauseLength) {
			return effectiveClauseLength <= _clause_store->getMaxAdmissibleEffectiveClauseLength();
		});
		_solvers[i]->setCallbackResultFound([&](int localId) {
			if (_det_sync) _det_sync->notifySolverDone(localId);
		});
		_solver_revisions.push_back(_solvers[i]->getSolverSetup().solverRevision);
		_solver_stats.push_back(&_solvers[i]->getSolverStatsRef());
	}

	if (_params.deterministicSolving()) {
		_det_sync.reset(new DeterministicClauseSynchronizer(_solvers, _num_original_clauses, [&](auto call) {
			onProduceClause(call.solverId, call.solverRevision, call.clause, call.condLits, true);
		}));
	}

	if (_job_index == 0 && _params.clauseLog.isSet()) {
		_clause_logger.reset(new ClauseLogger(_params.clauseLog()));
	}

	if (_params.groundTruthModel.isSet()) {
		std::ifstream ifs(_params.groundTruthModel());
		std::string modelStr;
		ifs >> modelStr;
		_groundtruth_model = ModelStringCompressor::decompress(modelStr);
	}
}

void SharingManager::onProduceClause(int solverId, int solverRevision, const Clause& clause, const std::vector<int>& condLits, bool recursiveCall) {

	if (!recursiveCall && _det_sync) {
		// Deterministic solving!
		_det_sync->insertBlocking(solverId, solverRevision, clause, condLits);
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
	if (!condLits.empty()) {
		tldClauseVec = new std::vector<int>(clause.size+condLits.size());
		for (int i = 0; i < clause.size; i++) tldClauseVec->at(i) = clause.begin[i];
		for (int i = clause.size; i < tldClauseVec->size(); i++)
			tldClauseVec->at(i) = condLits[i - clause.size];
		clauseBegin = tldClauseVec->data();
		clauseSize = tldClauseVec->size();
    }

	if (!_params.onTheFlyChecking()) { // otherwise already sorted
		// Sort literals in clause (not the metadata!)
		std::sort(clauseBegin+ClauseMetadata::numInts(), clauseBegin+clauseSize);
	}

	if (tldClauseVec) {
		// Check that the clause is sorted, remove duplicate lits
		int lit = INT_MIN;
		int shift = 0;
		for (int i = ClauseMetadata::numInts(); i < clauseSize; i++) {
			assert(clauseBegin[i] != 0);
			assert(clauseBegin[i] >= lit || log_return_false("[ERROR] Clause not sorted: %s\n", clause.toStr().c_str()));
			if (clauseBegin[i] == lit) shift++; // duplicate literal
			lit = clauseBegin[i];
			clauseBegin[i-shift] = lit;
		}
		clauseSize -= shift;
	}

	const int effectiveClauseLength = clauseSize - ClauseMetadata::numInts();

    // Check maximum size of clause
    if (effectiveClauseLength > _params.strictClauseLengthLimit()) {
        if (tldClauseVec) delete tldClauseVec;
        return;
    }

	if (effectiveClauseLength == 1 && clause.lbd != 1) {
		_logger.log(V1_WARN, "Observed unit LBD of %i\n", clause.lbd);
	}
	if (effectiveClauseLength > 1) {
		_observed_nonunit_lbd_of_zero |= clause.lbd == 0;
		_observed_nonunit_lbd_of_one |= clause.lbd == 1;
		_observed_nonunit_lbd_of_two |= clause.lbd == 2;
		_observed_nonunit_lbd_of_length_minus_one |= clause.lbd == clause.size-ClauseMetadata::numInts()-1;
		_observed_nonunit_lbd_of_length |= clause.lbd == clause.size-ClauseMetadata::numInts();
	}

	//log(V4_VVER, "EXPORT %s\n", clause.toStr().c_str());

	if (effectiveClauseLength == 1) assert(clause.lbd == 1);
	else {
		assert(clause.lbd >= 1 || LOG_RETURN_FALSE("[ERROR] len=%i lbd=%i!\n", effectiveClauseLength, clause.lbd));
		assert(clause.lbd <= effectiveClauseLength || LOG_RETURN_FALSE("[ERROR] len=%i lbd=%i!\n", effectiveClauseLength, clause.lbd));
	}
	int clauseLbd = effectiveClauseLength == 1 ? 1 :
		std::min(effectiveClauseLength, (int)std::max(2UL, clause.lbd + condLits.size()));

	if (_params.resetLbd() == MALLOB_RESET_LBD_AT_PRODUCE)
		clauseLbd = effectiveClauseLength;

	if (_params.incrementalVariableDomainHeuristic() >= 2) {
		// Rate the clause based on how many of its literals are "original",
		// i.e., within the range of original, rev.0 variables.
		clauseLbd = effectiveClauseLength == 1 ? 1 : 2;
		for (int i = ClauseMetadata::numInts(); i < clauseSize; i++) {
			// Each literal beyond the original variable range incurs a penalty.
			clauseLbd += (std::abs(clauseBegin[i]) > _num_original_vars);
		}
		// Clamp the "accumulated penalty" to the maximum valid LBD value.
		clauseLbd = std::min(clauseLbd, effectiveClauseLength);
	}

	// Add clause length to statistics
	_hist_produced.increment(clauseSize);
	auto& solverStats = _solver_stats[solverId];
	if (solverStats) {
		solverStats->producedClauses++;
		solverStats->histProduced->increment(clause.size);
	}

	if (!_groundtruth_model.empty()) {
		// Check whether the produced clause is satisfied by the ground truth
		bool ok = false;
		for (int i = ClauseMetadata::numInts(); i < clauseSize; i++) {
			int lit = clauseBegin[i];
			int var = std::abs(lit);
			if (var < _groundtruth_model.size() && _groundtruth_model[var] == lit) {
				ok = true;
				break;
			}
		} 
		if (!ok) {
			LOGGER(_logger, V0_CRIT, "[ERROR] Unable to satisfy learned clause %s with ground truth model!\n",
				Mallob::Clause(clauseBegin, clauseSize, clauseLbd).toStr().c_str());
			abort();
		}
	}

	_export_buffer->produce(clauseBegin, clauseSize, clauseLbd, solverId, _internal_epoch);
	//log(V6_DEBGV, "%i : PRODUCED %s\n", solverId, tldClause.toStr().c_str());

	if (tldClauseVec) delete tldClauseVec;
}

std::vector<int> SharingManager::prepareSharing(int totalLiteralLimit, int& outSuccessfulSolverId, int& outNbLits) {

	if (_det_sync) {
		if (!_det_sync->areAllSolversSyncReady()) return std::vector<int>();
		outSuccessfulSolverId = _det_sync->waitUntilSyncReadyAndReturnSolverIdWithResult();
		LOGGER(_logger, V4_VVER, "All solvers synced\n");
		if (outSuccessfulSolverId >= 0) {
			LOGGER(_logger, V4_VVER, "Emit successful solver ID %i\n", outSuccessfulSolverId);
		}
	}

	_sharing_op_ongoing = true;

	// Flushing the priority clause buffer results in owning locks
	// for an extended period, which may block solver threads.
	// Lock all filters such that solvers write to backlogs instead.
	float time = Timer::elapsedSeconds();
	_clause_filter->acquireAllLocks();
	time = Timer::elapsedSeconds() - time;
	LOGGER(_logger, V5_DEBG, "acquired all clause locks after %.6fs\n", time);

	int numExportedClauses = 0;
	auto buffer = _clause_store->exportBuffer(totalLiteralLimit, numExportedClauses, outNbLits,
			GenericClauseStore::ANY, /*sortClauses=*/true, [&](int* data) {

		// Shift clause ID from a local solver according to the solver's offset
		if (_id_alignment) {
			uint64_t id = ClauseMetadata::readUnsignedLong(data);
			assert(_id_alignment->isLocallyProducedClause(id) || log_return_false("[ERROR] Clause id=%lu is not locally produced!\n", id));
			_id_alignment->alignClauseId(data);
		}
	});

	_clause_filter->releaseAllLocks();

	// If desired, scramble the LBD scores of featured clauses
	if (_params.scrambleLbdScores()) {
		float time = Timer::elapsedSeconds();
		// 1. Create reader for shared clause buffer
		auto size = buffer.size();
		BufferReader reader(buffer.data(), size,
			_params.strictClauseLengthLimit()+ClauseMetadata::numInts(), false);
		// 2. Scramble clauses within each clause length w.r.t. LBD scores
		ClauseBufferLbdScrambler scrambler(_params, reader);
		auto modifiedClauseBuffer = scrambler.scrambleLbdScores();
		// 3. Overwrite clause buffer within our aggregation buffer
		buffer = std::move(modifiedClauseBuffer);
		assert(buffer.size() == size);
		time = Timer::elapsedSeconds() - time;
		LOGGER(_logger, V4_VVER, "scrambled LBDs in %.4fs\n", time);
	}

	LOGGER(_logger, V5_DEBG, "prepared %i clauses, size %i (%i in DB, limit %i)\n", numExportedClauses, buffer.size(), 
		_clause_store->getCurrentlyUsedLiterals(), totalLiteralLimit);
	_stats.exportedClauses += numExportedClauses;
	_internal_epoch++;
	_clause_filter->updateEpoch(_internal_epoch);

	return buffer;
}

void SharingManager::returnClauses(std::vector<int>& clauseBuf) {

	auto reader = _clause_store->getBufferReader(clauseBuf.data(), clauseBuf.size());

	// Lock all filters such that solvers write to backlogs instead.
	float time = Timer::elapsedSeconds();
	_clause_filter->acquireAllLocks();
	time = Timer::elapsedSeconds() - time;
	LOGGER(_logger, V5_DEBG, "acquired all clause locks after %.6fs\n", time);

	if (!_id_alignment) {
		// No clause ID alignments: Can just reinsert all clauses, no questions asked
		_clause_store->addClauses(reader, &_hist_returned_to_db);
	} else {
		auto c = reader.getNextIncomingClause();
		while (c.begin != nullptr) {

			// For certified UNSAT we need to drop returned clauses which do not
			// originate from this solver, since we can not un-align them to
			// correctly insert them into the database.
			auto id = ClauseMetadata::readUnsignedLong(c.begin);
			if (_id_alignment->isLocallyProducedClause(id)) {

				// Returned clauses would be aligned *again* when re-exported.
				// => subtract the offsets again here ...
				_id_alignment->unalignClauseId(c.begin);

				bool success = _clause_store->addClause(c);
				if (success) _hist_returned_to_db.increment(c.size);
			}

			c = reader.getNextIncomingClause();
		}
	}

	_clause_filter->releaseAllLocks(); // release filter locks again
}

std::vector<int> SharingManager::filterSharing(std::vector<int>& clauseBuf) {

	auto reader = _clause_store->getBufferReader(clauseBuf.data(), clauseBuf.size());
	auto id = _id_alignment ? _id_alignment->contributeFirstClauseIdOfEpoch() : 0UL;
	return FilterVectorBuilder(id, _internal_epoch, _job_index==0).build(reader, [&](Mallob::Clause& clause) {
		return _clause_filter->admitSharing(clause, _internal_epoch);
	}, [&](int len) {
		_clause_filter->acquireLock(len);
	}, [&](int len) {
		_clause_filter->releaseLock(len);
	});
}

void SharingManager::digestSharingWithFilter(std::vector<int>& clauseBuf, std::vector<int>* filter) {
	int verb = _job_index == 0 ? V3_VERB : V5_DEBG;

	float time = Timer::elapsedSeconds();
	ClauseHistogram hist(_params.strictClauseLengthLimit()+ClauseMetadata::numInts());

	_logger.log(verb, "digesting len=%ld\n", clauseBuf.size());

	std::vector<ImportingSolver> importingSolvers;
	for (size_t i = 0; i < _solvers.size(); i++) {
		auto& solver = _solvers[i];
		if (!solver || !_solver_stats[i]) continue; // solver was cleaned up
		if (!solver->isClauseSharingEnabled()) continue;
		assert(i == solver->getLocalId());
		importingSolvers.emplace_back(solver->getGlobalId(), solver->getLocalId(), _solver_stats[i], _id_alignment.get());
	}

	_last_num_cls_to_import = 0;
	_last_num_admitted_cls_to_import = 0;
	_last_num_admitted_lits_to_import = 0;

	if (importingSolvers.empty()) {
		_logger.log(verb, "no local solvers accepting clauses - ignoring sharing\n");
		_gc_pending = true;
		_sharing_op_ongoing = false;
		return;
	}

	// Apply provided global filter to buffer (in-place operation)
	applyFilterToBuffer(clauseBuf, filter);

	auto reader = _clause_store->getBufferReader(clauseBuf.data(), clauseBuf.size());

	_logger.log(verb+2, "DG import\n");

	// For each incoming clause (which was not filtered out)
	int filterSizeBeingLocked = -1;
	auto clause = reader.getNextIncomingClause();
	while (clause.begin != nullptr) {

		if (ClauseMetadata::enabled()) {
			assert(clause.size >= ClauseMetadata::numInts()+1 || log_return_false("[ERROR] Clause of invalid size %i!\n", clause.size));
			uint64_t id;
			memcpy(&id, clause.begin, sizeof(uint64_t));
		}

		if (_clause_logger) _clause_logger->append(clause);

		if (filterSizeBeingLocked != clause.size) {
			if (filterSizeBeingLocked != -1) _clause_filter->releaseLock(filterSizeBeingLocked);
			filterSizeBeingLocked = clause.size;
			_clause_filter->acquireLock(filterSizeBeingLocked);
		}

		hist.increment(clause.size);
		if (clause.size - ClauseMetadata::numInts() > _params.freeClauseLengthLimit())
			_last_num_admitted_lits_to_import += clause.size - ClauseMetadata::numInts();
		// bitset of producing solvers
		auto producers = _clause_filter->confirmSharingAndGetProducers(clause, _internal_epoch);

		if (_params.clauseErrorChancePerMille() > 0) {
			if (1000*Random::rand() <= _params.clauseErrorChancePerMille()) {
				// Tamper with a random (non meta data) literal
				_logger.log(V2_INFO, "TAMPERING with cls ID=%lu\n",
					ClauseMetadata::numInts()>=2 ? ClauseMetadata::readUnsignedLong(clause.begin) : 0);
				int idx = Random::rand() * (clause.size - ClauseMetadata::numInts());
				assert(idx >= 0); assert(idx < clause.size - ClauseMetadata::numInts());
				clause.begin[ClauseMetadata::numInts() + idx] += 1;
			}
		}

		// Decide for each solver whether it should receive the clause
		for (size_t i = 0; i < importingSolvers.size(); i++) {
			importingSolvers[i].appendCandidate(clause, producers);
		}

		clause = reader.getNextIncomingClause();
	}
	if (filterSizeBeingLocked != -1) _clause_filter->releaseLock(filterSizeBeingLocked);

	if (!_params.noImport()) {
		for (auto& slv : importingSolvers) {
			BufferReader reader = _clause_store->getBufferReader(clauseBuf.data(), clauseBuf.size());
			reader.setFilterBitset(slv.filter);
			_solvers[slv.localId]->addLearnedClauses(reader, _imported_revision);
		}
	}
	
	// Process-wide stats
	time = Timer::elapsedSeconds() - time;
	_logger.log(verb, "sharing time:%.4f adm:%i/%i %s\n", time, 
		_last_num_admitted_cls_to_import, _last_num_cls_to_import, hist.getReport().c_str());

	// Signal next garbage collection
	_gc_pending = true;
	_sharing_op_ongoing = false;
	if (_clause_logger) _clause_logger->publish();
}

void SharingManager::applyFilterToBuffer(std::vector<int>& clauseBuf, std::vector<int>* filter) {
	if (!filter) return;
	int verb = _job_index == 0 ? V3_VERB : V5_DEBG;

	_logger.log(verb+2, "DG apply global filter\n");
	const int bitsPerElem = sizeof(int)*8;
	int shift = bitsPerElem;
	int filterPos = -1 + (ClauseMetadata::enabled() ? 2 : 0);

	if (_id_alignment) {
		_id_alignment->beginNextEpoch(filter->data());
	}

	InPlaceClauseFiltering filtering(_params, clauseBuf.data(), clauseBuf.size(), filter->data(), filter->size());
	int buflen = filtering.applyAndGetNewSize();
	clauseBuf.resize(buflen);
	_last_num_cls_to_import += filtering.getNumClauses();
	_last_num_admitted_cls_to_import += filtering.getNumAdmittedClauses();
}

void SharingManager::digestSharingWithoutFilter(std::vector<int>& clauseBuf, bool stateless) {
	bool sharingOpOngoing = _sharing_op_ongoing;
	digestSharingWithFilter(clauseBuf, nullptr);
	if (stateless) _sharing_op_ongoing = sharingOpOngoing;
}

void SharingManager::digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>& clauseBuf) {
	// decide whether to perform the import
	int numUnknown = 0;
	for (int e = epochBegin; e < epochEnd; e++) {
		if (!_digested_epochs.count(e)) numUnknown++;
	}
	if (2*numUnknown >= epochEnd-epochBegin) {
		// More than half of the historic epochs are missing: do import.
		_logger.log(V2_INFO, "Import historic cls [%i,%i) (missing %i/%i)\n", 
			epochBegin, epochEnd, numUnknown, epochEnd-epochBegin);
		digestSharingWithoutFilter(clauseBuf, true);
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

	_logger.log(V5_DEBG, "Observed non-unit LBDs: 0:%i 1:%i 2:%i len-1:%i len:%i\n", 
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
	_solvers[solverId]->setCallbackResultFound([&](int localId) {
		if (_det_sync) _det_sync->notifySolverDone(localId);
	});
}

SharingManager::~SharingManager() {}

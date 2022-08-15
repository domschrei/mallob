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

#include "util/assert.hpp"

#include "sharing_manager.hpp"
#include "util/sys/timer.hpp"
#include "util/shuffle.hpp"
#include "buffer/buffer_reducer.hpp"

SharingManager::SharingManager(
		std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver, int jobIndex)
	: _solvers(solvers),
	_max_deferred_lits_per_solver(maxDeferredLitsPerSolver), 
	_params(params), _logger(logger), _job_index(jobIndex),
	_filter(params.clauseFilterClearInterval(), params.reshareImprovedLbd()),
	_cdb([&]() {
		AdaptiveClauseDatabase::Setup setup;
		setup.maxClauseLength = _params.strictClauseLengthLimit();
		setup.maxLbdPartitionedSize = _params.maxLbdPartitioningSize();
		setup.numLiterals = _params.clauseBufferBaseSize()*_params.numChunksForExport();
		setup.slotsForSumOfLengthAndLbd = _params.groupClausesByLengthLbdSum();
		return setup;
	}()), 
	_export_buffer(_filter, _cdb, _solver_stats, params.strictClauseLengthLimit()),
	_hist_produced(params.strictClauseLengthLimit()), 
	_hist_returned_to_db(params.strictClauseLengthLimit()) {

	_stats.histProduced = &_hist_produced;
	_stats.histFailedFilter = &_export_buffer.getFailedFilterHistogram();
	_stats.histAdmittedToDb = &_export_buffer.getAdmittedHistogram();
	_stats.histDroppedBeforeDb = &_export_buffer.getDroppedHistogram();
	_stats.histDeletedInSlots = &_cdb.getDeletedClausesHistogram();
	_stats.histReturnedToDb = &_hist_returned_to_db;

	auto callback = getCallback();
	
	assert(!solvers.empty());
	_num_original_clauses = solvers[0]->getSolverSetup().numOriginalClauses;
	_id_offsets_per_solver.resize(_solvers.size());
	_min_epoch_ids_per_solver.resize(_solvers.size());
	_last_exported_clause_id.resize(_solvers.size());
	unsigned long zerothEpochIdStart = _num_original_clauses+1;
	
	for (size_t i = 0; i < _solvers.size(); i++) {
		_solvers[i]->setExtLearnedClauseCallback(callback);
		_solver_revisions.push_back(_solvers[i]->getSolverSetup().solverRevision);
		_solver_stats.push_back(&_solvers[i]->getSolverStatsRef());

		//auto thisSolverEpochStart = _num_original_clauses + solvers[i]->getGlobalId()+1;

		_id_offsets_per_solver[i].push_back(0);
		_min_epoch_ids_per_solver[i].push_back(0);
		_last_exported_clause_id[i] = new std::atomic_ulong(0);

		LOG(V2_INFO, "EPOCH %i instance=%i prioroffset=%lu lastprodid=%lu startid=%lu\n", _min_epoch_ids_per_solver[i].size()-1, 
				_solvers[i]->getGlobalId(), _id_offsets_per_solver[i].back(), 0, _min_epoch_ids_per_solver[i].back());
	}
	_global_epoch_ids.push_back(0);
}

void SharingManager::onProduceClause(int solverId, int solverRevision, const Clause& clause, int condVarOrZero) {
		
	if (_solver_revisions[solverId] != solverRevision) return;

	if (_params.crashMonkeyProbability() > 0) {
		if (Random::rand() < _params.crashMonkeyProbability()) {
			// Crash!
			LOGGER(_logger, V3_VERB, "Simulating a crash!\n");
			raise(SIGSEGV); // causes crash
		}
	}

	if (MALLOB_CLAUSE_METADATA_SIZE == 2) {
		unsigned long clauseId = metadata::readUnsignedLong(clause.begin);
		_last_exported_clause_id[solverId]->store(clauseId, std::memory_order_relaxed);
		assert(getProducingLocalSolverIndex(clauseId) == solverId);
	}

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

	/*
	if (clauseSize == 1 && clause.lbd != 1) {
		_logger.log(V1_WARN, "Observed unit LBD of %i\n", clause.lbd);
	}
	if (clauseSize > 1) {
		_observed_nonunit_lbd_of_zero |= clause.lbd == 0;
		_observed_nonunit_lbd_of_one |= clause.lbd == 1;
		_observed_nonunit_lbd_of_two |= clause.lbd == 2;
		_observed_nonunit_lbd_of_length_minus_one |= clause.lbd == clause.size-1;
		_observed_nonunit_lbd_of_length |= clause.lbd == clause.size;
	}
	*/

	if (clauseSize == 1) assert(clause.lbd == 1);
	else {
		assert(clause.lbd >= 1 || LOG_RETURN_FALSE("[ERROR] len=%i lbd=%i!\n", clause.size, clause.lbd));
		assert(clause.lbd <= clause.size);
	}
	int clauseLbd = clauseSize == 1 ? 1 : std::max(2, clause.lbd + (condVarOrZero == 0 ? 0 : 1));

	// Add clause length to statistics
	_hist_produced.increment(clauseSize);
	auto& solverStats = _solver_stats[solverId];
	if (solverStats) {
		solverStats->producedClauses++;
		solverStats->histProduced->increment(clause.size);
	}

	_export_buffer.produce(clauseBegin, clauseSize, clauseLbd, solverId, _internal_epoch);
	//log(V6_DEBGV, "%i : PRODUCED %s\n", solverId, tldClause.toStr().c_str());

	/*
	auto result = _cdb.addClause(solverId, tldClause);

	if (result == SUCCESS) {
		_hist_admitted_to_db.increment(clauseSize);
		if (solverStats) solverStats->producedClausesAdmitted++;
	} else if (result == NO_BUDGET) {
		// completely dropping the clause
		_hist_dropped_before_db.increment(clauseSize);
		_stats.clausesDroppedAtExport++;
		if (solverStats) solverStats->producedClausesDropped++;
	} else {
		// duplicate
		_hist_failed_filter.increment(clauseSize);
		_stats.clausesProcessFilteredAtExport++;
		if (solverStats) solverStats->producedClausesSolverFiltered++;
	}
	*/

	if (tldClauseVec) delete tldClauseVec;
}

int SharingManager::prepareSharing(int* begin, int totalLiteralLimit) {

	int numExportedClauses = 0;
	auto buffer = _cdb.exportBuffer(totalLiteralLimit, numExportedClauses, 
			AdaptiveClauseDatabase::ANY, /*sortClauses=*/true, [&](int* data) {

		// Shift clause ID from a local solver according to the solver's offset
		if (MALLOB_CLAUSE_METADATA_SIZE == 2) alignClauseId(data);
	});
	//assert(buffer.size() <= maxSize);
	memcpy(begin, buffer.data(), buffer.size()*sizeof(int));

	LOGGER(_logger, V5_DEBG, "prepared %i clauses, size %i\n", numExportedClauses, buffer.size());
	_stats.exportedClauses += numExportedClauses;
	_internal_epoch++;

	return buffer.size();
}

void SharingManager::returnClauses(int* begin, int buflen) {

	auto reader = _cdb.getBufferReader(begin, buflen);
	auto c = reader.getNextIncomingClause();
	while (c.begin != nullptr) {

		// For certified UNSAT we need to drop returned clauses which do not
		// originate from this solver, since we can not un-align them to
		// correctly insert them into the database.  
		if (MALLOB_CLAUSE_METADATA_SIZE!=2 || isLocallyProducedClause(metadata::readUnsignedLong(c.begin))) {

			// Returned clauses would be aligned *again* when re-exported.
			// => subtract the offsets again here ...
			if (MALLOB_CLAUSE_METADATA_SIZE == 2) unalignClauseId(c.begin);

			bool success = _cdb.addClause(c);
			if (success) _hist_returned_to_db.increment(c.size);
		}

		c = reader.getNextIncomingClause();
	}
}

int SharingManager::filterSharing(int* begin, int buflen, int* filterOut) {

	auto reader = _cdb.getBufferReader(begin, buflen);
	
	constexpr auto bitsPerElem = 8*sizeof(int);
	int shift = bitsPerElem;
	auto clause = reader.getNextIncomingClause();
	int filterPos = -1 + (MALLOB_CLAUSE_METADATA_SIZE == 2 ? 2 : 0);
	int nbFiltered = 0;
	int nbTotal = 0;

	if (MALLOB_CLAUSE_METADATA_SIZE == 2) {
		// Proceed with the next epoch.
		// Find max. first clause ID
		unsigned long maxMinEpochId = 0;
		int maxNumSolvers = _solvers[0]->getSolverSetup().maxNumSolvers;
		for (size_t i = 0; i < _solvers.size(); i++) {
			auto clauseIdCounter = _last_exported_clause_id[i]->load(std::memory_order_relaxed);
			
			auto minEpochId = _id_offsets_per_solver[i].back() 
				+ clauseIdCounter
				+ maxNumSolvers;
			minEpochId = minEpochId - (minEpochId % maxNumSolvers) + maxNumSolvers;

			_min_epoch_ids_per_solver[i].push_back(clauseIdCounter);
			maxMinEpochId = std::max(maxMinEpochId, minEpochId);

			LOG(V2_INFO, "EPOCH %i instance=%i prioroffset=%lu lastprodid=%lu startid=%lu\n", _min_epoch_ids_per_solver[i].size()-1, 
				_solvers[i]->getGlobalId(), _id_offsets_per_solver[i].back(), clauseIdCounter, _min_epoch_ids_per_solver[i].back());
		}
		metadata::writeUnsignedLong(maxMinEpochId, filterOut);
	}

	_filter.acquireLock();
	while (clause.begin != nullptr) {
		++nbTotal;

		if (shift == bitsPerElem) {
			++filterPos;
			filterOut[filterPos] = 0;
			shift = 0;
		}
		
		if (!_filter.admitSharing(clause, _internal_epoch)) {
			// filtered!
			auto bitFiltered = 1 << shift;
			filterOut[filterPos] |= bitFiltered;
			++nbFiltered;
		}
		
		++shift;
		clause = reader.getNextIncomingClause();
	}
	_filter.releaseLock();

	_logger.log(V4_VVER, "filtered %i/%i\n", nbFiltered, nbTotal);
	return filterPos+1;
}

void SharingManager::digestSharingWithFilter(int* begin, int buflen, const int* filter) {

	int verb = _job_index == 0 ? V3_VERB : V5_DEBG;
	float time = Timer::elapsedSeconds();
	ClauseHistogram hist(_params.strictClauseLengthLimit());

	_logger.log(verb, "digesting len=%ld\n", buflen);

	std::vector<PortfolioSolverInterface*> importingSolvers;
	for (auto& solver : _solvers) {
		if (solver->getCurrentRevision() == _current_revision) {
			importingSolvers.push_back(solver.get());
		}
	}

	_last_num_cls_to_import = 0;
	_last_num_admitted_cls_to_import = 0;

	if (MALLOB_CLAUSE_METADATA_SIZE == 2) assert(filter != nullptr);

	// Apply provided global filter to buffer (in-place operation)
	if (filter != nullptr) {
		_logger.log(verb+2, "DG apply global filter\n");
		const int bitsPerElem = sizeof(int)*8;
		int shift = bitsPerElem;
		int filterPos = -1 + (MALLOB_CLAUSE_METADATA_SIZE == 2 ? 2 : 0);
		
		if (MALLOB_CLAUSE_METADATA_SIZE == 2) {
			// extract global min. epoch ID and compute the next ID offset
			// for each solver from it
			unsigned long globalMinEpochId = metadata::readUnsignedLong(filter);
			LOG(V2_INFO, "EPOCH %i GLOBAL_MAX_OF_1ST_ID %lu\n", _min_epoch_ids_per_solver[0].size()-1, globalMinEpochId);
			_global_epoch_ids.push_back(globalMinEpochId);

			for (size_t i = 0; i < _solvers.size(); i++) {

				auto numSolvers = _solvers[i]->getSolverSetup().maxNumSolvers;
				auto offset = globalMinEpochId - _min_epoch_ids_per_solver[i].back();
				offset = offset - (offset % numSolvers) + numSolvers;
				_id_offsets_per_solver[i].push_back(offset);
				
				auto testClauseId = _last_exported_clause_id[i]->load(std::memory_order_relaxed) + _solvers[i]->getSolverSetup().maxNumSolvers;
				alignClauseId((int*) &testClauseId);
				LOG(V2_INFO, "EPOCH %i instance=%i newoffset=%lu nextalignedclauseid=%lu\n", 
					_min_epoch_ids_per_solver[i].size()-1, _solvers[i]->getGlobalId(), _id_offsets_per_solver[i].back(),
					testClauseId);
			}
		}

		BufferReducer reducer(begin, buflen, _params.strictClauseLengthLimit(), _params.groupClausesByLengthLbdSum());
		buflen = reducer.reduce([&]() {
			_last_num_cls_to_import++;
			if (shift == bitsPerElem) {
				filterPos++;
				shift = 0;
			}
			bool admitted = ((filter[filterPos] & (1 << shift)) == 0);
			if (admitted) {
				_last_num_admitted_cls_to_import++;
			}
			shift++;
			return admitted;
		});
	}

	// Prepare to traverse clauses not filtered yet
	std::vector<std::forward_list<int>> unitLists(importingSolvers.size());
	std::vector<std::forward_list<std::pair<int, int>>> binaryLists(importingSolvers.size());
	std::vector<std::forward_list<std::vector<int>>> largeLists(importingSolvers.size());
	std::vector<int> currentCapacities(importingSolvers.size(), -1);
	std::vector<int> currentAddedLiterals(importingSolvers.size(), 0);

	auto reader = _cdb.getBufferReader(begin, buflen);
	BufferIterator it(_params.strictClauseLengthLimit(), /*slotsForSumOfLengthAndLbd=*/false);
	auto clause = reader.getNextIncomingClause();
	bool explicitLbds = false;

	// Method to publish completed clause lists
	auto doPublishClauseLists = [&]() {
		if (it.clauseLength == 1) {
			// Publish unit lists
			for (size_t i = 0; i < importingSolvers.size(); i++) {
				if (!unitLists[i].empty()) {
					importingSolvers[i]->addLearnedClauses(it.clauseLength, it.lbd, unitLists[i], currentAddedLiterals[i]);
				}
			}
		} else if (it.clauseLength == 2) {
			// Publish binary lists
			for (size_t i = 0; i < importingSolvers.size(); i++) {
				if (!binaryLists[i].empty()) {
					importingSolvers[i]->addLearnedClauses(it.clauseLength, it.lbd, binaryLists[i], currentAddedLiterals[i]);
				}
			}
		} else {
			// Publish large lists
			for (size_t i = 0; i < importingSolvers.size(); i++) {
				if (!largeLists[i].empty()) {
					importingSolvers[i]->addLearnedClauses(it.clauseLength, it.lbd, largeLists[i], currentAddedLiterals[i]);
				}
			}
		}
	};

	_logger.log(verb+2, "DG prepare import\n");

	// Traverse clauses
	bool initialized = false;
	_filter.acquireLock();

	_logger.log(verb+2, "DG import\n");

	while (clause.begin != nullptr) {
		
		if (!initialized || clause.size != it.clauseLength || clause.lbd != it.lbd) {
			initialized = true;
			float publishTime = Timer::elapsedSeconds();
			_filter.releaseLock();

			doPublishClauseLists();

			while (clause.size != it.clauseLength || clause.lbd != it.lbd) {
				it.nextLengthLbdGroup();
				explicitLbds = it.storeWithExplicitLbd(/*maxLbdPartitioningSize=*/2);
			}

			for (size_t i = 0; i < importingSolvers.size(); i++) {
				currentCapacities[i] = importingSolvers[i]->getClauseImportBudget(clause.size, clause.lbd);
				currentAddedLiterals[i] = 0;
			}

			_filter.acquireLock();
			publishTime = Timer::elapsedSeconds() - publishTime;
			_logger.log(verb+2, "DG published clause lists (%.4f s)\n", publishTime);
		}

		hist.increment(clause.size);
		uint8_t producers = _filter.getProducers(clause, _internal_epoch);

		for (size_t i = 0; i < importingSolvers.size(); i++) {
			auto& solver = *importingSolvers[i];
			int sid = solver.getLocalId();
			auto& solverStats = _solver_stats[sid];
			solverStats->receivedClauses++;
			if (currentCapacities[i] < clause.size) {
				// No import budget left
				solverStats->receivedClausesDropped++;
				continue;
			}
			uint8_t producerFlag = 1 << sid;
			if ((producers & (1 << sid)) != 0) {
				// filtered by solver filter
				solverStats->receivedClausesFiltered++;
				continue;
			} else {
				if (MALLOB_CLAUSE_METADATA_SIZE == 2 && clause.size >= 2) {
					// check via clause ID whether this solver produced this clause
					unsigned long clauseId = metadata::readUnsignedLong(clause.begin);
					if (getProducingInstanceId(clauseId) == solver.getGlobalId()) {
						// This solver produced this clause! Do not import.
						solverStats->receivedClausesFiltered++;
						abort();
						continue;
					}
					// Important invariant: incoming clauses must be from EARLIER epochs
					// than your current epoch.
					int epoch = _min_epoch_ids_per_solver[i].size()-1;
					int clauseEpoch = metadata::getEpoch(clauseId, _global_epoch_ids);
					if (clauseEpoch >= epoch) {
						LOG(V0_CRIT, "[ERROR] Importing clause ID=%lu from epoch %i while I am in epoch %i myself!\n", 
							clauseId, clauseEpoch, epoch);
						abort();
					}
				}
				// admitted by solver filter
				if (clause.size == 1) unitLists[i].push_front(clause.begin[0]);
				else if (clause.size == 2) binaryLists[i].emplace_front(clause.begin[0], clause.begin[1]);
				else {
					std::vector<int> clauseVec((explicitLbds ? 1 : 0) + clause.size);
					size_t idx = 0;
					if (explicitLbds) clauseVec[idx++] = clause.lbd;
					for (size_t k = 0; k < clause.size; k++) clauseVec[idx++] = clause.begin[k];
					largeLists[i].emplace_front(std::move(clauseVec));
				}
				currentCapacities[i] -= clause.size;
				currentAddedLiterals[i] += clause.size;
			}
		}

		clause = reader.getNextIncomingClause();
	}
	_filter.releaseLock();
	doPublishClauseLists();
	
	// Process-wide stats
	time = Timer::elapsedSeconds() - time;
	_logger.log(verb, "sharing time:%.4f adm:%i/%i %s\n", time, 
		_last_num_admitted_cls_to_import, _last_num_cls_to_import, hist.getReport().c_str());
}

void SharingManager::digestSharingWithoutFilter(int* begin, int buflen) {
	digestSharingWithFilter(begin, buflen, nullptr);
}

SharingStatistics SharingManager::getStatistics() {
	/*
	_logger.log(V2_INFO, "Observed non-unit LBDs: 0:%i 1:%i 2:%i len-1:%i len:%i\n", 
		_observed_nonunit_lbd_of_zero, 
		_observed_nonunit_lbd_of_one, 
		_observed_nonunit_lbd_of_two, 
		_observed_nonunit_lbd_of_length_minus_one, 
		_observed_nonunit_lbd_of_length);
	*/
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

int SharingManager::getEpochOfUnalignedSelfClause(unsigned long id) {
	auto producingSolver = getProducingLocalSolverIndex(id);
	auto& epochList = _min_epoch_ids_per_solver[producingSolver];
	// will point to 1st element >= id (or end)
	auto it = std::lower_bound(epochList.begin(), epochList.end(), id);
	assert(it != epochList.begin());
	// point to last element < id
	--it;
	return std::distance(epochList.begin(), it);
}
int SharingManager::getEpochOfAlignedSelfClause(unsigned long id) {
	auto& epochList = _global_epoch_ids;
	// will point to 1st element >= id (or end)
	auto it = std::lower_bound(epochList.begin(), epochList.end(), id);
	assert(it != epochList.begin());
	// point to last element < id
	--it;
	return std::distance(epochList.begin(), it);
}

void SharingManager::writeClauseEpochs(const std::string& proofDir, int firstGlobalId, 
		const std::string& outputFilename) {
	
	std::string tempFilename = outputFilename + "~";
	std::ofstream ofs(tempFilename);
	ofs << _num_original_clauses << "\n";

	for (int epoch = 0; epoch < _global_epoch_ids.size(); epoch++) {

		// Check if all necessary entries for this epoch are present
		if ([&]() {
			for (size_t i = 0; i < _id_offsets_per_solver.size(); i++)
				if (epoch >= _min_epoch_ids_per_solver[i].size() || epoch >= _id_offsets_per_solver[i].size())
					return true; // cancel writing
			return false; // continue writing
		}()) break;

		ofs << epoch << " " << _global_epoch_ids[epoch];
		for (size_t i = 0; i < _id_offsets_per_solver.size(); i++) {
			ofs << " " << _min_epoch_ids_per_solver[i][epoch];
			ofs << " " << _id_offsets_per_solver[i][epoch];
		}
		ofs << "\n";
	}

	/*
	// Wait until all proof files are written sufficiently
	for (size_t i = 0; i < _id_offsets_per_solver.size(); i++) {

		auto lastRelevantId = _last_exported_clause_id[i]->load(std::memory_order_relaxed);
		if (lastRelevantId <= _num_original_clauses) continue;

		std::string proofFilename = proofDir + "/proof." 
				+ std::to_string(firstGlobalId+i+1) + ".frat";

		// Wait until the solver's proof file has been written up to this clause ID
		while (true) {

			// Open process which yields the last two lines of the proof file
			std::string cmd = "tail -2 " + proofFilename;
			FILE* tailProcess = popen(cmd.c_str(), "r");
			char lineBuffer[65536];
			bool doneWaiting = false;

			// Read output from process
			while (!feof(tailProcess)) {
				// Try to parse next line
				if (fgets(lineBuffer, 65536, tailProcess) == NULL) continue;
				unsigned long writtenId = 0;
				for (size_t i = 0; i < 65536; i++) {
					auto c = lineBuffer[i];
					if (c == '\n') break; // end of line
					if (c == ' ') {
						if (writtenId != 0) break; // ID read completely
						continue; // no ID read yet
					}
					if (c >= '0' && c <= '9') {
						// continue to parse ID
						writtenId = writtenId*10 + (c-'0');
					}
				}
				LOG(V2_INFO, "Proof \"%s\" : ID %lu found\n", proofFilename.c_str(), writtenId);
				if (writtenId < lastRelevantId) continue;
				// Done!
				doneWaiting = true;
			}
			if (doneWaiting) {
				LOG(V2_INFO, "Proof \"%s\" now contains ID %lu\n", proofFilename.c_str(), lastRelevantId);
				break;
			}

			LOG(V2_INFO, "Still waiting for proof \"%s\" to contain ID %lu\n", 
				proofFilename.c_str(), lastRelevantId);
			usleep(1000 * 200); // wait 0.2s
		}
	}
	*/

	LOG(V2_INFO, "renaming clause epochs file ...\n");
	std::rename(tempFilename.c_str(), outputFilename.c_str());
	LOG(V2_INFO, "wrote clause epochs file for distributed proof assembly\n");
}

SharingManager::~SharingManager() {}

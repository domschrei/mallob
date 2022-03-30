/*
 * AllToAllSharingManager.cpp
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#include <signal.h>

#include "util/assert.hpp"

#include "sharing_manager.hpp"
#include "util/sys/timer.hpp"
#include "util/shuffle.hpp"

SharingManager::SharingManager(
		std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, 
		const Parameters& params, const Logger& logger, size_t maxDeferredLitsPerSolver, int jobIndex)
	: _solvers(solvers),
	_max_deferred_lits_per_solver(maxDeferredLitsPerSolver), 
	_params(params), _logger(logger), _job_index(jobIndex),
	_filter(params.clauseFilterClearInterval()),
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
	
    for (size_t i = 0; i < _solvers.size(); i++) {
		_solvers[i]->setExtLearnedClauseCallback(callback);
		_solver_revisions.push_back(_solvers[i]->getSolverSetup().solverRevision);
		_solver_stats.push_back(&_solvers[i]->getSolverStatsRef());
	}
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
	int clauseLbd = clauseSize == 1 ? 1 : std::max(2, clause.lbd);

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
	auto buffer = _cdb.exportBuffer(totalLiteralLimit, numExportedClauses);
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
		bool success = _cdb.addClause(c);
		if (success) _hist_returned_to_db.increment(c.size);
		c = reader.getNextIncomingClause();
	}
}

int SharingManager::filterSharing(int* begin, int buflen, int* filterOut) {

	auto reader = _cdb.getBufferReader(begin, buflen);
	
	constexpr auto bitsPerElem = 8*sizeof(int);
	int shift = bitsPerElem;
	auto clause = reader.getNextIncomingClause();
	int filterPos = -1;
	int nbFiltered = 0;
	int nbTotal = 0;

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

	// Get all clauses
	auto reader = _cdb.getBufferReader(begin, buflen);

	_logger.log(verb, "digesting len=%ld\n", buflen);

	std::vector<PortfolioSolverInterface*> importingSolvers;
	for (auto& solver : _solvers) {
		if (solver->getCurrentRevision() == _current_revision) {
			importingSolvers.push_back(solver.get());
		}
	}

	const int bitsPerElem = sizeof(int)*8;
	int shift = bitsPerElem;
	int filterPos = -1;
	_last_num_cls_to_import = 0;
	_last_num_admitted_cls_to_import = 0;

	_filter.acquireLock();
	auto clause = reader.getNextIncomingClause();
	while (clause.begin != nullptr) {
		
		_last_num_cls_to_import++;
		if (shift == bitsPerElem) {
			filterPos++;
			shift = 0;
		}

		auto bit = 1 << shift;
		if (filter == nullptr || ((filter[filterPos] & bit) == 0)) {

			// admitted
			_last_num_admitted_cls_to_import++;
			hist.increment(clause.size);
			auto producers = _filter.getInfo(clause, _internal_epoch).producers;

			for (auto solver : importingSolvers) {
				int sid = solver->getLocalId();
				auto& solverStats = _solver_stats[sid];
				solverStats->receivedClauses++;
				uint8_t producerFlag = 1 << sid;
				if ((producers & producerFlag) != 0) {
					// filtered
					solverStats->receivedClausesFiltered++;
					continue;
				} else {
					// admitted
					_solvers[sid]->addLearnedClause(clause);
				}
			}
		}

		clause = reader.getNextIncomingClause();
		shift++;
	}
	_filter.releaseLock();
	
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

SharingManager::~SharingManager() {}

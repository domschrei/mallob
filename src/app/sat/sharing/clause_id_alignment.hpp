
#pragma once

#include "app/sat/data/clause.hpp"
#include "app/sat/solvers/portfolio_solver_interface.hpp"
#include "util/logger.hpp"
#include <fstream>
#include <memory>

/*
Clause ID alignment structure for distributed proof production with alignment
of clause IDs at each sharing epoch.
*/
class ClauseIdAlignment {

private:
    const Logger& _logger;
    std::vector<std::shared_ptr<PortfolioSolverInterface>>& _solvers;
    const int _num_original_clauses;
    const int _max_nb_threads_per_process;

	std::vector<std::atomic_ulong*> _last_exported_clause_id; 
	typedef std::vector<unsigned long> EpochIdList;
	std::vector<EpochIdList> _min_epoch_ids_per_solver;
	std::vector<EpochIdList> _id_offsets_per_solver;
	EpochIdList _global_epoch_ids;

public:
    ClauseIdAlignment(const Logger& logger, std::vector<std::shared_ptr<PortfolioSolverInterface>>& solvers, int nbOrigClauses, int maxNbThreadsPerProcess) :
            _logger(logger), _solvers(solvers), _num_original_clauses(nbOrigClauses), _max_nb_threads_per_process(maxNbThreadsPerProcess) {

        _id_offsets_per_solver.resize(_solvers.size());
        _min_epoch_ids_per_solver.resize(_solvers.size());
        _last_exported_clause_id.resize(_solvers.size());
        
        for (size_t i = 0; i < _solvers.size(); i++) {

            _id_offsets_per_solver[i].push_back(0);
            _min_epoch_ids_per_solver[i].push_back(0);
            _last_exported_clause_id[i] = new std::atomic_ulong(_num_original_clauses+1);

            LOGGER(_logger, V3_VERB, "EPOCH %i instance=%i prioroffset=%lu lastprodid=%lu startid=%lu\n", _min_epoch_ids_per_solver[i].size()-1, 
                    _solvers[i]->getGlobalId(), _id_offsets_per_solver[i].back(), _last_exported_clause_id[i]->load(std::memory_order_relaxed), 
                    _min_epoch_ids_per_solver[i].back());
        }

        _global_epoch_ids.push_back(0);
    }

    void onProduceClause(const Mallob::Clause& clause, int solverId) {
        unsigned long clauseId = ClauseMetadata::readUnsignedLong(clause.begin);
        assert(clauseId > _num_original_clauses);
        assert(isLocallyProducedClause(clauseId));
		assert(getProducingLocalSolverIndex(clauseId) == solverId);
		_last_exported_clause_id[solverId]->store(clauseId, std::memory_order_relaxed);
    }

    void contributeFirstClauseIdOfEpoch(int* bufferPositionOut) {
        // Proceed with the next epoch.
		// Find max. first clause ID
		unsigned long maxFirstIdOfEpoch = 0;
		int maxNumSolvers = _solvers[0]->getSolverSetup().maxNumSolvers;
		for (size_t i = 0; i < _solvers.size(); i++) {

			auto clauseIdCounter = _last_exported_clause_id[i]->load(std::memory_order_relaxed);
			_min_epoch_ids_per_solver[i].push_back(clauseIdCounter);
			
			auto firstIdOfEpoch = _id_offsets_per_solver[i].back() 
				+ clauseIdCounter
				+ maxNumSolvers;
			firstIdOfEpoch = firstIdOfEpoch - (firstIdOfEpoch % maxNumSolvers) + maxNumSolvers;
			maxFirstIdOfEpoch = std::max(maxFirstIdOfEpoch, firstIdOfEpoch);

			LOGGER(_logger, V3_VERB, "EPOCH %i instance=%i prioroffset=%lu lastprodid=%lu startid=%lu\n", _min_epoch_ids_per_solver[i].size()-1, 
				_solvers[i]->getGlobalId(), _id_offsets_per_solver[i].back(), clauseIdCounter, _min_epoch_ids_per_solver[i].back());
		}

		ClauseMetadata::writeUnsignedLong(maxFirstIdOfEpoch, bufferPositionOut);
    }

    void beginNextEpoch(const int* bufferWithMaxFirstIdOfEpoch) {

        auto numSolvers = _solvers[0]->getSolverSetup().maxNumSolvers;
		unsigned long globalMinEpochId = ClauseMetadata::readUnsignedLong(bufferWithMaxFirstIdOfEpoch);
		globalMinEpochId = std::max(globalMinEpochId, _global_epoch_ids.back() + numSolvers);
		LOGGER(_logger, V3_VERB, "EPOCH %i GLOBAL_MAX_OF_1ST_ID %lu\n", _min_epoch_ids_per_solver[0].size()-1, globalMinEpochId);
		assert(globalMinEpochId > _num_original_clauses);
		_global_epoch_ids.push_back(globalMinEpochId);

		for (size_t i = 0; i < _solvers.size(); i++) {

			auto offset = globalMinEpochId - _min_epoch_ids_per_solver[i].back();
			offset = offset - (offset % numSolvers) + numSolvers;
			_id_offsets_per_solver[i].push_back(offset);

			LOGGER(_logger, V3_VERB, "EPOCH %i instance=%i newoffset=%lu\n", 
				_min_epoch_ids_per_solver[i].size()-1, _solvers[i]->getGlobalId(), _id_offsets_per_solver[i].back());
		}
    }

    bool checkClauseToImport(PortfolioSolverInterface* solver, const Mallob::Clause& clause) {
        // check via clause ID whether this solver produced this clause
        unsigned long clauseId = ClauseMetadata::readUnsignedLong(clause.begin);
        if (getProducingInstanceId(clauseId) == solver->getGlobalId()) {
            // This solver produced this clause! Do not import.
            return false;
        }
        // Important invariant: incoming clauses must be from EARLIER epochs
        // than your current epoch.
        int i = solver->getLocalId();
        int epoch = _min_epoch_ids_per_solver[i].size()-1;
        int clauseEpoch = ClauseMetadata::getEpoch(clauseId, _global_epoch_ids);
        if (clauseEpoch >= epoch) {
            LOGGER(_logger, V0_CRIT, "[ERROR] Importing clause ID=%lu from epoch %i while I am in epoch %i myself!\n", 
                clauseId, clauseEpoch, epoch);
            abort();
        }
        return true;
    }

	bool isLocallyProducedClause(unsigned long clauseId) {
		auto globalId = getProducingInstanceId(clauseId);
		for (auto& solver : _solvers) if (solver->getGlobalId() == globalId) return true;
		return false;
	}

	int getProducingLocalSolverIndex(unsigned long clauseId) {
		return (clauseId-_num_original_clauses) % _max_nb_threads_per_process;
	}
	int getProducingInstanceId(unsigned long clauseId) {
		return (clauseId-_num_original_clauses) % _solvers[0]->getSolverSetup().maxNumSolvers;
	}

    void alignClauseId(int* clauseData) {

		unsigned long clauseId = ClauseMetadata::readUnsignedLong(clauseData);
		int localSolverId = getProducingLocalSolverIndex(clauseId);

		// take the offset that belongs to the clause's epoch!
		int epoch = getEpochOfUnalignedSelfClause(clauseId);
		assert(epoch >= 0 && epoch < _id_offsets_per_solver[localSolverId].size() 
			|| log_return_false("Invalid epoch %i found for clause ID %lu\n", epoch, clauseId));
		auto offset = _id_offsets_per_solver[localSolverId][epoch];
		unsigned long alignedClauseId = clauseId + offset;

		LOGGER(_logger, V5_DEBG, "ALIGN EPOCH=%i %lu => %lu\n", epoch, clauseId, alignedClauseId);

		assert(getEpochOfAlignedSelfClause(alignedClauseId) == getEpochOfUnalignedSelfClause(clauseId));
		assert(getProducingLocalSolverIndex(alignedClauseId) == getProducingLocalSolverIndex(clauseId));

		ClauseMetadata::writeUnsignedLong(alignedClauseId, clauseData);
	}

	void unalignClauseId(int* clauseData) {

		unsigned long clauseId = ClauseMetadata::readUnsignedLong(clauseData);
		int localSolverId = getProducingLocalSolverIndex(clauseId);

		int epoch = getEpochOfAlignedSelfClause(clauseId);
		assert(epoch >= 0 && epoch < _id_offsets_per_solver[localSolverId].size() 
			|| log_return_false("Invalid epoch %i found for clause ID %lu\n", epoch, clauseId));
		auto offset = _id_offsets_per_solver[localSolverId][epoch];
		unsigned long unalignedClauseId = clauseId - offset;

		LOGGER(_logger, V5_DEBG, "UNALIGN EPOCH=%i %lu => %lu\n", epoch, clauseId, unalignedClauseId);

		assert(getEpochOfAlignedSelfClause(clauseId) == getEpochOfUnalignedSelfClause(unalignedClauseId) 
			|| log_return_false("[ERROR] epoch of aligned clause %lu: %i; epoch of unaligned clause %lu: %i\n",
			clauseId, getEpochOfAlignedSelfClause(clauseId), unalignedClauseId, getEpochOfUnalignedSelfClause(unalignedClauseId)));
		assert(getProducingLocalSolverIndex(clauseId) == getProducingLocalSolverIndex(unalignedClauseId));

		ClauseMetadata::writeUnsignedLong(unalignedClauseId, clauseData);
	}

    int getEpochOfUnalignedSelfClause(unsigned long id) {
        auto producingSolver = getProducingLocalSolverIndex(id);
        auto& epochList = _min_epoch_ids_per_solver[producingSolver];
        // will point to 1st element >= id (or end)
        auto it = std::lower_bound(epochList.begin(), epochList.end(), id);
        assert(it != epochList.begin());
        //if (it == epochList.end() || *it > id) {
            // point to last element < id
            --it;
        //}
        return std::distance(epochList.begin(), it);
    }

    int getEpochOfAlignedSelfClause(unsigned long id) {
        auto& epochList = _global_epoch_ids;
        // will point to 1st element >= id (or end)
        auto it = std::lower_bound(epochList.begin(), epochList.end(), id);
        assert(it != epochList.begin());
        //if (it == epochList.end() || *it > id) {
            // point to last element < id
            --it;
        //}
        return std::distance(epochList.begin(), it);
    }
		
	unsigned long getGlobalStartOfSuccessEpoch() {
		return _global_epoch_ids.empty() ? 0 : _global_epoch_ids.back();
	}

    void writeClauseEpochs(/*const std::string& proofDir, int firstGlobalId, */
            const std::string& outputFilename) {

        std::string tempFilename = outputFilename + "~";
        {
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
        }

        LOGGER(_logger, V3_VERB, "renaming clause epochs file ...\n");
        std::rename(tempFilename.c_str(), outputFilename.c_str());
        LOGGER(_logger, V3_VERB, "wrote clause epochs file for distributed proof assembly\n");
    }

};

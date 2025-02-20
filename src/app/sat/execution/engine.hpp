
#pragma once

#include <ext/alloc_traits.h>
#include <future>
#include <list>
#include <stddef.h>
#include <atomic>
#include <vector>
#include <memory>

#include "app/sat/data/revision_data.hpp"
#include "app/sat/data/theories/integer_rule.hpp"
#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "../sharing/sharing_manager.hpp"
#include "solver_thread.hpp"
#include "solving_state.hpp"
#include "util/params.hpp"
#include "data/checksum.hpp"
#include "data/job_result.hpp"
#include "../job/sat_process_config.hpp"

class Logger;
class PortfolioSolverInterface;
class SharingManager;
struct SolverSetup;

class SatEngine {

private:

	Parameters _params;
	SatProcessConfig _config;
	Logger& _logger;
	int _job_id;
	
	size_t _num_solvers;
	size_t _num_active_solvers;
	
	std::unique_ptr<SharingManager> _sharing_manager;
	std::vector<std::shared_ptr<PortfolioSolverInterface>> _solver_interfaces;
	std::vector<std::shared_ptr<SolverThread>> _solver_threads;
	std::vector<std::shared_ptr<SolverThread>> _obsolete_solver_threads;
	std::list<std::future<void>> _solver_thread_cleanups;

	std::vector<RevisionData> _revision_data;
	
	bool _solvers_started = false;
	volatile SolvingStates::SolvingState _state;
	int _revision = -1;
	JobResult _result;
	std::atomic_bool _cleaned_up = false;

	bool _block_result {false};
	int _winning_solver_id {-1};

	std::vector<std::pair<long, int>> _objective;

public:

    SatEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface);
	~SatEngine();

	void solve();
	void setClauseBufferRevision(int revision);
	void appendRevision(int revision, RevisionData data, bool lastRevisionForNow = true);

	bool isFullyInitialized();
	int solveLoop();
	JobResult& getResult() {return _result;}

	bool isReadyToPrepareSharing() const;
	std::vector<int> prepareSharing(int literalLimit, int& outSuccessfulSolverId, int& outNbLits);
	std::vector<int> filterSharing(std::vector<int>& clauseBuf);
	void addSharingEpoch(int epoch);
	void digestSharingWithFilter(std::vector<int>& clauseBuf, std::vector<int>& filter);
	void digestSharingWithoutFilter(std::vector<int>& clauseBuf, bool stateless);
	void returnClauses(std::vector<int>& clauseBuf);
	void digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>& clauseBuf);

	struct LastAdmittedStats {
		int nbAdmittedCls;
		int nbTotalCls;
		int nbAdmittedLits;
	};
	LastAdmittedStats getLastAdmittedClauseShare();
	long long getBestFoundObjectiveCost() const;
	void updateBestFoundObjectiveCost(long long bestFoundObjectiveCost);

	void setWinningSolverId(int globalId);
	void syncDeterministicSolvingAndCheckForLocalWinner();

	void reduceActiveThreadCount();
	void setActiveThreadCount(int nbThreads);

    void setPaused();
    void unsetPaused();
	void terminateSolvers(bool hardTermination = false);

	const Parameters& getParams() {return _params;}
	void dumpStats(bool final);
	std::vector<long> getSolverTids() {
		std::vector<long> tids;
		for (size_t i = 0; i < _solver_threads.size(); i++) {
			if (_solver_threads[i]->isInitialized()) 
				tids.push_back(_solver_threads[i]->getTid());
		}
		return tids;
	}

	void cleanUp(bool hardTermination = false);
	bool isCleanedUp() {return _cleaned_up;}

private:

	void writeClauseEpochs();
	std::shared_ptr<PortfolioSolverInterface> createSolver(const SolverSetup& setup);
};

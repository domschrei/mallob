
#pragma once

#include <atomic>
#include <vector>
#include <memory>

#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "../sharing/sharing_manager.hpp"
#include "solver_thread.hpp"
#include "solving_state.hpp"
#include "util/params.hpp"
#include "data/checksum.hpp"
#include "data/job_result.hpp"
#include "../job/sat_process_config.hpp"

class SatEngine {

private:

	Parameters _params;
	SatProcessConfig _config;
	Logger& _logger;
	int _job_id;
	
	size_t _num_solvers;
	
	std::unique_ptr<SharingManager> _sharing_manager;
	std::vector<std::shared_ptr<PortfolioSolverInterface>> _solver_interfaces;
	std::vector<std::shared_ptr<SolverThread>> _solver_threads;
	std::vector<std::shared_ptr<SolverThread>> _obsolete_solver_threads;

	struct RevisionData {
		size_t fSize;
		const int* fLits;
		size_t aSize;
		const int* aLits;
	};
	std::vector<RevisionData> _revision_data;
	
	bool _solvers_started = false;
	volatile SolvingStates::SolvingState _state;
	int _revision = -1;
	JobResult _result;
	std::atomic_bool _cleaned_up = false;

public:

    SatEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface);
	~SatEngine();

	void solve();
    void appendRevision(int revision, size_t fSize, const int* fLits, size_t aSize, const int* aLits, 
		bool lastRevisionForNow = true);

	bool isFullyInitialized();
    int solveLoop();
	JobResult& getResult() {return _result;}

    int prepareSharing(int* begin, int maxSize);
	int filterSharing(int* begin, int size, int* filterOut);
	void digestSharingWithFilter(int* begin, int size, const int* filter);
	void digestSharingWithoutFilter(int* begin, int size);
	void returnClauses(int* begin, int size);
	std::pair<int, int> getLastAdmittedClauseShare();

    void setPaused();
    void unsetPaused();
	void terminateSolvers();

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

	void cleanUp();
	bool isCleanedUp() {return _cleaned_up;}

private:

	std::shared_ptr<PortfolioSolverInterface> createSolver(const SolverSetup& setup);

};

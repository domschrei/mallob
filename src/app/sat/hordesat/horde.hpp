/*
 * HordeLib.h
 *
 *  Created on: Mar 24, 2017
 *      Author: balyo
 */

#ifndef HORDELIB_H_
#define HORDELIB_H_

#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <set>
#include <map>

#include "util/sys/threading.hpp"
#include "util/logger.hpp"
#include "sharing/sharing_manager_interface.hpp"
#include "solvers/solver_thread.hpp"
#include "solvers/solving_state.hpp"
#include "util/params.hpp"
#include "data/job_result.hpp"

class HordeLib {

private:

	Parameters _params;
	Logger _logger;
	
	size_t _sleep_microsecs;
	size_t _num_solvers;
	
	std::unique_ptr<SharingManagerInterface> _sharing_manager;
	std::vector<std::shared_ptr<PortfolioSolverInterface>> _solver_interfaces;
	std::vector<std::shared_ptr<SolverThread>> _solver_threads;
	
	bool _solvers_started = false;
	volatile SolvingStates::SolvingState _state;
	int _revision = -1;
	JobResult _result;
	std::atomic_bool _cleaned_up = false;

public:

    HordeLib(const Parameters& params, Logger&& loggingInterface);
	~HordeLib();

    void appendRevision(int revision, size_t fSize, const int* fLits, size_t aSize, const int* aLits);
	void solve();

	bool isFullyInitialized();
    int solveLoop();
	JobResult& getResult() {return _result;}

    int prepareSharing(int* begin, int maxSize);
    void digestSharing(std::vector<int>& result);
	void digestSharing(int* begin, int size);

    void interrupt();
	void setSolvingState(SolvingStates::SolvingState state);
    void setPaused();
    void unsetPaused();
	void abort();

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
};

#endif /* HORDELIB_H_ */

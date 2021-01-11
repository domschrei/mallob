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

class HordeLib {

private:

	Parameters _params;
	Logger _logger;
	
	size_t _sleep_microsecs;
	size_t _num_solvers;

	// Payload to solve
	std::vector<std::shared_ptr<std::vector<int>>> _formulae;
	std::shared_ptr<std::vector<int>> _assumptions;
	
	std::unique_ptr<SharingManagerInterface> _sharing_manager;
	std::vector<std::shared_ptr<PortfolioSolverInterface>> _solver_interfaces;
	std::vector<std::shared_ptr<SolverThread>> _solver_threads;
	
	volatile SolvingStates::SolvingState _state;
	SatResult _result;
	std::vector<int> _model;
	std::set<int> _failed_assumptions;
	std::atomic_bool _solution_found = false;
	std::atomic_bool _cleaned_up = false;

public:

    HordeLib(const Parameters& params, Logger&& loggingInterface);
	~HordeLib();

    void beginSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
							const std::shared_ptr<std::vector<int>>& assumptions);
	void continueSolving(const std::vector<std::shared_ptr<std::vector<int>>>& formulae, 
							const std::shared_ptr<std::vector<int>>& assumptions);
	void updateRole(int rank, int numNodes);
	bool isFullyInitialized();
	bool isAnySolutionFound() {return _solution_found;}
    int solveLoop();

    int prepareSharing(int* begin, int maxSize);
    void digestSharing(const std::vector<int>& result);
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

	int value(int lit);
	int failed(int lit);
	std::vector<int>& getTruthValues() {
		return _model;
	}
	std::set<int>& getFailedAssumptions() {
		return _failed_assumptions;
	}

	void cleanUp();
	bool isCleanedUp() {return _cleaned_up;}	
};

#endif /* HORDELIB_H_ */

/*
 * LogSharingManager.h
 *
 *  Created on: Apr 1, 2015
 *      Author: balyo
 */

#ifndef SHARING_LOGSHARINGMANAGER_H_
#define SHARING_LOGSHARINGMANAGER_H_

#include "AllToAllSharingManager.h"

class LogSharingManager: public virtual AllToAllSharingManager {
private:
	int exchangeCount;
public:
	LogSharingManager(int mpi_size, int mpi_rank, vector<PortfolioSolverInterface*> solvers,
			ParameterProcessor& params);
    void init(int size);
	void doSharing();
    std::vector<int> prepareSharing(int size);
    void digestSharing();
    void digestSharing(const std::vector<int>& result);
	virtual ~LogSharingManager();
};

#endif /* SHARING_LOGSHARINGMANAGER_H_ */

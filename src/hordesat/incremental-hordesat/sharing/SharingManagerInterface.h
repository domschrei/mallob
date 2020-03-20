/*
 * SharingManagerInterface.h
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#ifndef SHARING_SHARINGMANAGERINTERFACE_H_
#define SHARING_SHARINGMANAGERINTERFACE_H_

#include "../solvers/PortfolioSolverInterface.h"

struct SharingStatistics {
	SharingStatistics():sharedClauses(0),importedClauses(0),filteredClauses(0),dropped(0) {}
	unsigned long sharedClauses;
	unsigned long importedClauses;
	unsigned long filteredClauses;
	unsigned long dropped;
};

class SharingManagerInterface {

public:
	virtual std::vector<int> prepareSharing(int maxSize) = 0;
    virtual void digestSharing(const std::vector<int>& result) = 0;

    virtual SharingStatistics getStatistics() = 0;
	virtual ~SharingManagerInterface() {};

};



#endif /* SHARING_SHARINGMANAGERINTERFACE_H_ */

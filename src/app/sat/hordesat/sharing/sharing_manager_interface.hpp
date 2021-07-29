/*
 * SharingManagerInterface.h
 *
 *  Created on: Mar 5, 2015
 *      Author: balyo
 */

#ifndef SHARING_SHARINGMANAGERINTERFACE_H_
#define SHARING_SHARINGMANAGERINTERFACE_H_

#include "app/sat/hordesat/solvers/portfolio_solver_interface.hpp"
#include "data/checksum.hpp"

struct SharingStatistics {
	SharingStatistics():exportedClauses(0),importedClauses(0),
		clausesDroppedAtExport(0),
		clausesFilteredAtExport(0),
		clausesFilteredAtImport(0) {}
	unsigned long exportedClauses;
	unsigned long importedClauses;
	unsigned long clausesDroppedAtExport;
	unsigned long clausesFilteredAtExport;
	unsigned long clausesFilteredAtImport;
	unsigned long* seenClauseLenHistogram;
};

class SharingManagerInterface {

public:
	virtual int prepareSharing(int* begin, int maxSize) = 0;
    virtual void digestSharing(std::vector<int>& result) = 0;
    virtual void digestSharing(int* begin, int size) = 0;

    virtual SharingStatistics getStatistics() = 0;
	virtual ~SharingManagerInterface() {};

	virtual void stopClauseImport(int solverId) = 0;
	virtual void continueClauseImport(int solverId) = 0;
	virtual void setRevision(int revision) = 0;
};



#endif /* SHARING_SHARINGMANAGERINTERFACE_H_ */

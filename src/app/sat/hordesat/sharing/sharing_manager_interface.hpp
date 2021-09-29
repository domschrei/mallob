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
#include "app/sat/hordesat/sharing/clause_histogram.hpp"

struct SharingStatistics {
	unsigned long exportedClauses = 0;
	unsigned long clausesDroppedAtExport = 0;
	unsigned long clausesDeferredAtExport = 0;
	unsigned long clausesProcessFilteredAtExport = 0;
	unsigned long clausesSolverFilteredAtExport = 0;
	unsigned long importedClauses = 0;
	ClauseHistogram* histProduced;
	ClauseHistogram* histAdmittedToDb;
	ClauseHistogram* histDroppedBeforeDb;
	ClauseHistogram* histDeletedInSlots;
	ClauseHistogram* histReturnedToDb;

	std::string getReport() const {
		unsigned long failedExported = clausesProcessFilteredAtExport + clausesSolverFilteredAtExport + clausesDroppedAtExport;
		unsigned long exportedWithFailed = exportedClauses + failedExported;
		float droppedRatio = failedExported == 0 ? 0 : (float)clausesDroppedAtExport / failedExported;

		return "exp:" + std::to_string(exportedClauses) + "/" + std::to_string(exportedWithFailed)
			+ " drp:" + std::to_string(clausesDroppedAtExport) 
					+ "(" + std::to_string((float) (0.01 * (int)(droppedRatio*100))) + ")"
			+ " dfr:" + std::to_string(clausesDeferredAtExport)
			+ " pflt:" + std::to_string(clausesProcessFilteredAtExport)
			+ " sflt:" + std::to_string(clausesSolverFilteredAtExport)
			+ " imp:" + std::to_string(importedClauses);
	}
};

class SharingManagerInterface {

public:
	virtual int prepareSharing(int* begin, int maxSize) = 0;
    virtual void digestSharing(std::vector<int>& result) = 0;
    virtual void digestSharing(int* begin, int size) = 0;
	virtual void returnClauses(int* begin, int buflen) = 0;

    virtual SharingStatistics getStatistics() = 0;
	virtual ~SharingManagerInterface() {};

	virtual void stopClauseImport(int solverId) = 0;
	virtual void continueClauseImport(int solverId) = 0;
	virtual void setRevision(int revision) = 0;
};



#endif /* SHARING_SHARINGMANAGERINTERFACE_H_ */


#pragma once

struct SharingStatistics {

	unsigned long exportedClauses = 0;
	unsigned long clausesDroppedAtExport = 0;
	unsigned long clausesProcessFilteredAtExport = 0;
	unsigned long clausesSolverFilteredAtExport = 0;
	ClauseHistogram* histProduced;
	ClauseHistogram* histFailedFilter;
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
			+ " pflt:" + std::to_string(clausesProcessFilteredAtExport)
			+ " sflt:" + std::to_string(clausesSolverFilteredAtExport);
	}
};

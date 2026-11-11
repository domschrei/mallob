#ifndef MALLOB_MUSENGINE_HPP
#define MALLOB_MUSENGINE_HPP

#include "app/sat/execution/scatterengine.hpp"

class MusEngine : public ScatterEngine {

public:
	static constexpr Jobtype KICK_CANDIDATE_CLAUSE{0};
    static constexpr Jobtype ROTATE_MODEL_AROUND_CLAUSE{1};

	struct solverdata {
		// every solver stores an own copy of Astatus,
		// necessary to know at the end of a job the exact starting conditions
		std::vector<char> Astatus{};
		int Afixed_first_unread_pos{0};
		int last_revision_nr;
	};


private:
	SharedMutex _Mutex_status_fixed;
	std::vector<char> _Astatus{};
	std::vector<int> _Afixed{};


	SharedMutex _Mutex_found_locally;
	std::vector<int> _Aness_found_locally{};
	std::vector<int> _Acore_found_locally{};

	SharedMutex _Mutex_pos;
	int _Astatus_next_candidate_pos{0};
	std::atomic<bool> _noCandidatesLeft{false};

	std::vector<solverdata> _solverdata{};

	int _numAssumptions{-1};
	int _firstAssumpVar{-1};

public:
	MusEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface);
	~MusEngine() {
		if (!isCleanedUp()) cleanUp();
	}
	virtual void init() override;

	// public virtual overrides
    int getNumSolvers(int numThreads) override;
	std::string getSolverChoicesStr() override;

	/* our data to export */		 std::vector<int> scattershareExport(std::vector<int> &&request_for_export) override;
	/* our feedback to the import */ std::vector<int> scattershareImport(std::vector<int> &&data_to_import) override;

protected:
	// private virtual overrides
	std::vector<JobToSolverMapping> getJobToSolverMappingDefinitions() override;
	Scatterjob createNewDefaultJob(int preferred_id, SolverBackendType solvertype) override;
	void readScatterjobResult(int reporting_id, Jobtype jobtype, const JobResult &result, const Scatterjob &job) override;
	void instantiateJob(Scatterjob &job) override;
	Scatterjob createNewJobFromResult(int reporting_id, Jobtype old_jobtype, JobResult &&old_result, const Scatterjob &old_job) override;
	std::vector<int> gatherScattersharing() override;
	void digestScattersharing(std::vector<int> &&model) override;

private:
	//Own MUS methods
	int getNextCandidateVar();
	int posToVar(int pos);
	int varToPos(int var);
	void rebalanceWork();
	void newNessAssumpFoundLocally(int a);
	void newGloballyFixedAssumptions(std::vector<int> &&Afixed);
	char sign(int a);
	bool hasCandidateAssumpVar(const Scatterjob &job);
	void setCandidateAssumpVar(Scatterjob &job, int a);
	int getCandidateAssumpVar(const Scatterjob &job);
	int getCoreUndecidedSizeLocked(const std::vector<int> &core);
	void newCoreFoundLocally(std::vector<int> &&core);
	Scatterjob createNewRotationJob(int a_ness, std::vector<int> &&model);
	void fixVariablesOutsideCore_MOVE_TO_CLIENT_LATER();
	void interruptOutdatedJobs(std::vector<int> &&Afixed);


};

#endif //MALLOB_MUSENGINE_HPP

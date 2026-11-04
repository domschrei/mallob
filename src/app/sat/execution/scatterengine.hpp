//
// Created by nicco on 11/4/26.
//

#ifndef MALLOB_SCATTERENGINE_HPP
#define MALLOB_SCATTERENGINE_HPP
#include <queue>
#include <unordered_map>

#include "app/sat/solvers/solver_portfolio_config.hpp"
#include "app/sat/execution/solver_thread.hpp"

typedef std::shared_ptr<SolverThread> solverptr;

class ScatterEngine {
public:
	//Defining the jobtype as a class allows the custom scatter engines to define their own enums
	//while we can still write generic code about them without knowing the custom enums
	enum class Jobtype : int {};

	struct Scatterjob {
		size_t nr{0};
		Jobtype jobtype;
		int preferred_id{0};
		int revision{-1};
		std::vector<int> definition{};
		std::vector<int> payload{};
		int running_id{-1};
		bool has_claimed_solver{false};
	};

	struct JobToSolverMapping {
		Jobtype jobtype;
		SolverBackendType solvertype;
	};


private:
	Parameters _params;
	bool _initialized{false};

	std::vector<solverptr> &_solver_threads; //not owned, just reference

	std::unordered_map<Jobtype, std::queue<Scatterjob>> _scatterjob_queues{}; //one queue per jobtype
	std::mutex _scatterjob_queues_mutex;

	std::vector<Scatterjob> _scatterjobs_running{}; //at most one running job per solver id
	// std::vector<JobToSolverMapping> _allowed_mappings{}; //allowed matches from jobtype to solvertype
	std::unordered_map<SolverBackendType, std::vector<Jobtype>> _mapped_jobtypes_to_solvertype{};

public:
	ScatterEngine(std::vector<solverptr> &solver_threads, Parameters &params);
	virtual ~ScatterEngine() = default;

private:
	//these methods are already implemented by ScatterEngine itself
	void init();
	bool doesJobMapToSolver(Jobtype jobtype, SolverBackendType solvertype) const;
	// SolverBackendType getType(SolverThread &solverthread) const;
	int loop();
	Scatterjob getJobForSolver(solverptr solver);
	bool tryDeployJob(Scatterjob job);
	void deployNewJobForSolverOrRelease(solverptr solver);
	solverptr claimFreeSolverOrNull(int preferred_id, Jobtype jobtype);
	void storeJobAsRunning(Scatterjob &&job);
	ScatterEngine::Scatterjob &&extractStoredRunningJob(int reporting_id);
	void requeueJob(Scatterjob &job);
	void cbScatterjobResult(int reporting_id, Jobtype jobtype);
	bool tryChainNextJob(int reporting_id, Jobtype jobtype, JobResult &&result);
	bool isJobObsolete(Scatterjob &job);




	/*
	 * The following methods need to be implemented by each custom engine
	 */
	//Provide a list which specifies for every custom jobtype the solvertype on which this jobtype must run
	//Each jobtype must map to exactly one solvertype, but each solvertype can be mapped to by multiple jobtypes
	virtual std::vector<JobToSolverMapping> getMappingDefinitions() const = 0;

	//Provide the desired solver combinations, using the -satsolver= logic
	virtual std::string_view getSolverChoicesStr() const = 0;

	//Provide the number of created solver. This can also be more than the number of cores,
	//since its the responsibility of the custom engine to decide when to run which specific solver
	virtual int getNumSolversForCores(int numCores) const = 0;

	//Provide a new default job if nothing else is known or given
	//Can return obsolete jobs for some solvertypes as long as it returns a proper job for at least one solvertype
	virtual Scatterjob createNewDefaultJob(solverptr solver) const = 0;

	//Turn the definition of a job into actual payload, potentially using the fact that now we know
	//the exact solver this job will run on (via job.running_id)
	//Must also provide a correct job.revision number for that given solver
	virtual void instantiateJobForSpecificSolver(Scatterjob &job) const = 0;

	//Do something with the obtained result
	virtual void readScatterjobResult(int reporting_id, Jobtype jobtype, JobResult &result) const = 0;

	//Given a result, a special next job can be returned that builds directly on top of the result
	virtual Scatterjob getSpecialChainedJobOrDummy(int reporting_id, Jobtype jobtype, JobResult &&result) const = 0;

};


#endif //MALLOB_SCATTERENGINE_HPP

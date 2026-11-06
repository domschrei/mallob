//
// Created by nicco on 11/4/26.
//

#ifndef MALLOB_SCATTERENGINE_HPP
#define MALLOB_SCATTERENGINE_HPP
#include <queue>
#include <unordered_map>

#include "app/sat/execution/engine.hpp"
#include "app/sat/solvers/solver_portfolio_config.hpp"
#include "app/sat/execution/solver_thread.hpp"

typedef std::shared_ptr<SolverThread> solverptr;

class ScatterEngine : public SatEngine {
public:
	//class integer enums allow the children (MusEngine, ...) to define their own enums
	//while we can still write generic code about them here
	enum class Jobtype : int {};

	struct Scatterjob {
		size_t nr{0};
		Jobtype jobtype;
		int revision{-1};
		int preferred_id{0};
		int running_id{-1};
		std::vector<int> definition{};
		std::vector<int> payload{};
	};

	struct JobToSolverMapping {
		Jobtype jobtype;
		SolverBackendType solvertype;
	};

protected:

	int _numThreads{-1};
	std::atomic_size_t _num_scattersolvers_currently_running{0};

	//one jobqueue per jobtype
	std::unordered_map<Jobtype, std::queue<Scatterjob>> _scatterjob_queues{};
	Mutex _mutex_scatterjob_queues;

	//the jobs currently running, indexed via their running solvers id
	std::vector<Scatterjob> _scatterjobs_stored_running{};
	Mutex _mutex_scatterjobs_running;

	//specifies which jobtypes are to be run on which solvertypes
	std::unordered_map<SolverBackendType, std::vector<Jobtype>> _mapped_jobtypes_per_solvertype{};

	//the scatter engine automatically increments revision counts, if not overwritten by the custom engine
	std::vector<int> _revision_counts;

	//Decide whether the job payload should be std::move'd into the revision (faster, but no longer accessible later)
	//or whether it should only be copied (slower, but still accessible later when reading results)
	bool _should_job_payload_be_moved_out_of_job = true;

public:
	ScatterEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface);
	virtual ~ScatterEngine() = default;
	virtual void init() override;

	//todo: more overrrides of standard SatEngine methods
	//	or at least modifications to them

protected:
	bool doesJobMapToSolver(Jobtype jobtype, SolverBackendType solvertype) const;
	void loop();
	Scatterjob getJob(int preferred_id, SolverBackendType solvertype);
	void tryToRunJob(Scatterjob &&job);
	solverptr claimFreeSolverOrNull(int preferred_id, Jobtype jobtype);
	void storeJobAsRunning(Scatterjob &&job);
	Scatterjob extractStoredFinishedJob(int reporting_id);
	void requeueJob(Scatterjob &&job);
	void cbScatterjobResult(int reporting_id, Jobtype jobtype);
	bool isEmpty(Scatterjob &job);
	int incrementRevisionFallback(Scatterjob &job);
	// bool isRunning(Scatterjob &job);


	/*
	 * The following methods need to be implemented by each custom engine
	 */
	//Provide a list which specifies for every custom jobtype the solvertype on which this jobtype must run
	//Each jobtype must map to exactly one solvertype, but each solvertype can be mapped to by multiple jobtypes
	virtual std::vector<JobToSolverMapping> getJobToSolverMappingDefinitions() const = 0;

	//Provide a new default job if nothing else is known or given
	//Can return obsolete jobs for some solvertypes as long as it returns a proper job for at least one solvertype
	virtual Scatterjob createNewDefaultJob(int preferred_id, SolverBackendType solvertype) const = 0;

	//Turn the definition of a job into actual payload
	//This is done on purpose later than the job definition, because here at instantiation
	//we now know the exact solver id this job will run on, which can be necessary information to make the payload.
	//This method must also provide a valid job.revision number for that given running solver
	virtual void instantiateJob(Scatterjob &job) const = 0;

	//Do something with the obtained result. Can also be Unknown, if solver got e.g. interrupted.
	virtual void readScatterjobResult(int reporting_id, Jobtype jobtype, const JobResult &result, const Scatterjob &job) const = 0;

	//Given a result, a special next job can be returned that builds directly on top of the result
	virtual Scatterjob createNewJobFromResult(int reporting_id, Jobtype jobtype, JobResult &&old_result, const Scatterjob &old_job) {
		//default: no-op, returns an empty job
		return Scatterjob();
	}

public:
	//Provide the desired solver combinations using the logic of the -satsolver=... flag
	//e.g. "kcl" for kissat cadical lingeling repeating
	virtual std::string getSolverChoicesStr() override = 0;

	//Provide the number of solvers that should be created. This can also be more than the number of cores,
	//since not all solvers will run all of the time, the ScatterEngine tries to handle this
	virtual int getNumSolvers(int numCores) override = 0;

};


#endif //MALLOB_SCATTERENGINE_HPP

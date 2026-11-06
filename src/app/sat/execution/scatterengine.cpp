//// Created by nicco on 11/4/26.
//

#include "scatterengine.hpp"

#include "engine.hpp"

ScatterEngine::ScatterEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface)
	: SatEngine(params, config, loggingInterface)
{
	//empty
}

void ScatterEngine::init() {
	//create the solvers etc.
	SatEngine::init();
	//pre-compute the job-to-solver mappings for more ergonomic iteration during solving
	for (auto &map : getJobToSolverMappingDefinitions()) {
		_mapped_jobtypes_per_solvertype[map.solvertype].push_back(map.jobtype);
		_scatterjob_queues[map.jobtype]; //initialize for each jobtype
	}
	//Allocation for every created solver
	_scatterjobs_stored_running.resize(_num_solvers);
	_revision_counts.resize(_num_solvers);

	//Remember the actual number of threads (or physical cores) that are available
	_numThreads = _config.threads;
}

void ScatterEngine::loop() {
	//if terminated: do we need to retun early, to avoid touching terminating solvers?
	if (_num_scattersolvers_currently_running >= _numThreads)
		return;
	for (auto &solver : _solver_threads) {
		if (!solver) continue;
		if (!solver->isClaimedForScatterJob()) {
			auto job = getJob(solver->getLocalId(), solver->getSolvertype());
			tryToRunJob(std::move(job));
		}
	}
}

ScatterEngine::Scatterjob ScatterEngine::getJob(int preferred_id, SolverBackendType solvertype) {
	{
		//look first in queue(s) if there exist any job for this solvertype
		auto lock = _mutex_scatterjob_queues.getLock();
		for (auto jobtype : _mapped_jobtypes_per_solvertype.at(solvertype)) {
			auto &queue = _scatterjob_queues.at(jobtype);
			if (!queue.empty()) {
				auto job = std::move(queue.front()); queue.pop();
				job.preferred_id = preferred_id;
				return job;
			}
		}
	}
	//otherwise create a new job
	return createNewDefaultJob(preferred_id, solvertype);
}

//returns the ID of the running solver, or -1 otherwise
void ScatterEngine::tryToRunJob(Scatterjob &&job) {
	// constexpr int NOT_RUNNING = -1;
	if (isEmpty(job))
		return;
	solverptr solver = claimFreeSolverOrNull(job.preferred_id, job.jobtype);
	if (!solver) {
		requeueJob(std::move(job));
		return;
	}
	job.running_id = solver->getLocalId();
	instantiateJob(job);
	if (isEmpty(job)) {
		//we noticed after instantiation that *now* the job is no longer relevant
		solver->releaseScatterjobClaim();
		return;
	}
	//Need the revision here because we move the job out
	int revision = incrementRevisionFallback(job);
	RevisionData revisionData = _should_job_payload_be_moved_out_of_job
			? RevisionData{std::make_shared<std::vector<int>>(std::move(job.payload)), {}}
			: RevisionData{std::make_shared<std::vector<int>>(job.payload), {}};
	//Need to store before solver starts solving
	storeJobAsRunning(std::move(job));
	solver->appendRevision(revision, revisionData);
	return;
}

int ScatterEngine::incrementRevisionFallback(Scatterjob &job) {
	if (job.revision < 0) {
		//in case that the user didn't bother setting a revision
		//pre-increment, because revision 0 is already taken when setting up the solver
		job.revision = ++_revision_counts[job.running_id];
	}
	return job.revision;
}

solverptr ScatterEngine::claimFreeSolverOrNull(int preferred_id, Jobtype jobtype) {
	if (_num_scattersolvers_currently_running >= _numThreads)
		return nullptr;
	const size_t n = _solver_threads.size();
	if (n == 0) return nullptr;
	//loop over all IDs, starting with the preferred one
	for (size_t j = 0; j < n; j++) {
		const size_t id = (preferred_id + j) % n;
		const solverptr solver = _solver_threads[id];
		if (solver && doesJobMapToSolver(jobtype, solver->getSolvertype()) && solver->claimForScatterjob()) {
			return solver;
		}
	}
	return nullptr;
}

bool ScatterEngine::doesJobMapToSolver(Jobtype jobtype, SolverBackendType solvertype) const {
	for (auto mapped_jobtype : _mapped_jobtypes_per_solvertype.at(solvertype)) {
		if (jobtype == mapped_jobtype) {
			return true;
		}
	}
	return false;
}

bool ScatterEngine::isEmpty(Scatterjob &job) {
	//We use this emptiness property both to signal that a job became useless/obsolete
	//or that is just a dummy placeholder
	return job.definition.empty() && job.payload.empty();
}

// bool ScatterEngine::isRunning(Scatterjob &job) {
	// return job.running_id != -1 ;
// }

void ScatterEngine::storeJobAsRunning(Scatterjob &&job) {
	auto lock = _mutex_scatterjobs_running.getLock();
	//Slot should be empty
	assert(isEmpty(_scatterjobs_stored_running[job.running_id]));
	//Store
	_scatterjobs_stored_running[job.running_id] = std::move(job);
	_num_scattersolvers_currently_running++;
}

ScatterEngine::Scatterjob ScatterEngine::extractStoredFinishedJob(int reporting_id) {
	auto lock = _mutex_scatterjobs_running.getLock();
	//Copy out stored job
	auto stored_job = _scatterjobs_stored_running[reporting_id];
	assert(stored_job.running_id == reporting_id);
	assert(!isEmpty(stored_job));
	//Fill with empty dummy
	_scatterjobs_stored_running[reporting_id] = Scatterjob();
	_num_scattersolvers_currently_running--;
	return stored_job;
}

void ScatterEngine::requeueJob(Scatterjob &&job) {
	auto lock = _mutex_scatterjob_queues.getLock();
	_scatterjob_queues.at(job.jobtype).push(std::move(job));
}

void ScatterEngine::cbScatterjobResult(int reporting_id, Jobtype jobtype) {
	auto finished_job = extractStoredFinishedJob(reporting_id);
	auto solver = _solver_threads[reporting_id];
	assert(solver->isClaimedForScatterJob());
	//Careful! The solver thread does NOT update its result member when the solver finishes without finding SAT or UNSAT
	//(e.g. when interrupted). So we need to check the revision number of the stored result to see whether it is actually
	//relevant for us, or just outdated junk
	JobResult result;  //starts with state UNKOWN
	auto &tentative_result = solver->getSatResult();
	if (tentative_result.revision == finished_job.revision) {
		result = std::move(tentative_result);
	}
	readScatterjobResult(reporting_id, jobtype, result, finished_job);
	// assert(result.revision == finished_job.revision);
	//engine specific readout of the results
	//We release the solver because it is simpler and more robust than keeping claims which span over multiple nested
	//function calls. It also keeps up the invariant that every new job has to claim its own solver on the spot.
	//The only small downside is that this way we create a very short time period where the main loop could snatch the
	//now-unclaimed solver. This is however not a real problems since the job created here would then just be regularly queued.
	solver->releaseScatterjobClaim();
	//immediately chain a next job into the system
	if (_params.scatterChain()) {
		//now that we have a result, it might be that we want to create a special job based on that result
		auto new_job = createNewJobFromResult(reporting_id, jobtype, std::move(result), finished_job);
		if (isEmpty(new_job)) {
			//no result-dependent job was created, fallback to a generic job
			new_job = getJob(reporting_id, solver->getSolvertype());
		}
		tryToRunJob(std::move(new_job));
	}
}


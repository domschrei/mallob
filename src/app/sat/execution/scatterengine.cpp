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
	//pre-compute the mappings (by e.g MusEngine) to iterate more elegantly over them during solving
	for (auto &map : getJobToSolverMappingDefinitions()) {
		_mapped_jobtypes_per_solvertype.at(map.solvertype).push_back(map.jobtype);
	}
	_scatterjobs_stored_running.resize(_num_solvers);
}

void ScatterEngine::loop() {
	//if terminated: do we need to retun early, to avoid touching terminating solvers?
	if (_num_scattersolvers_currently_running >= _config.threads)
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
	//look first in queue(s) if there exist any job for this solvertype
	for (auto jobtype : _mapped_jobtypes_per_solvertype.at(solvertype)) {
		auto &queue = _scatterjob_queues[jobtype];
		//Almost always the queue will be empty anyways, cheaper to check without mutex
		if (!queue.empty()) {
			//Rare case, now we try again with a proper exclusive mutexed check
			auto lock = _mutex_scatterjob_queues.getLock();
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
int ScatterEngine::tryToRunJob(Scatterjob &&job) {
	constexpr int NOT_RUNNING = -1;
	if (isEmpty(job))
		return NOT_RUNNING;
	solverptr solver = claimFreeSolverOrNull(job.preferred_id, job.jobtype);
	if (!solver) {
		requeueJob(std::move(job));
		return NOT_RUNNING;
	}
	job.running_id = solver->getLocalId();
	instantiateJob(job);
	if (isEmpty(job)) {
		//we noticed after instantiation that *now* the job is no longer relevant
		solver->releaseScatterjobClaim();
		return NOT_RUNNING;
	}
	solver->appendRevision(job.revision, {std::make_shared<std::vector<int>>(std::move(job.payload)), {}});
	storeJobAsRunning(std::move(job));
	return solver->getLocalId();
}

solverptr ScatterEngine::claimFreeSolverOrNull(int preferred_id, Jobtype jobtype) {
	if (_num_scattersolvers_currently_running >= _config.threads)
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
	return job.definition.empty() && job.payload.empty();
}

void ScatterEngine::storeJobAsRunning(Scatterjob &&job) {
	//The slot where we want to store the job should be empty (or filled with a dummy job)
	auto lock = _mutex_scatterjobs_running.getLock();
	assert( _scatterjobs_stored_running[job.running_id].running_id==-1);
	_scatterjobs_stored_running[job.running_id] = std::move(job);
	_num_scattersolvers_currently_running++;
}

ScatterEngine::Scatterjob &&ScatterEngine::extractStoredFinishedJob(int reporting_id) {
	auto lock = _mutex_scatterjobs_running.getLock();
	assert(_scatterjobs_stored_running[reporting_id].running_id == reporting_id);
	_num_scattersolvers_currently_running--;
	return std::move(_scatterjobs_stored_running[reporting_id]);
}

void ScatterEngine::requeueJob(Scatterjob &&job) {
	auto lock = _mutex_scatterjob_queues.getLock();
	_scatterjob_queues[job.jobtype].push(std::move(job));
}

void ScatterEngine::cbScatterjobResult(int reporting_id, Jobtype jobtype) {
	auto finished_job = std::move(extractStoredFinishedJob(reporting_id));
	auto solver = _solver_threads[reporting_id];
	assert(solver->isClaimedForScatterJob());
	assert(solver->hasFoundResult(finished_job.revision));
	auto result = std::move(solver->getSatResult());
	//engine specific readout of the results
	readScatterjobResult(reporting_id, jobtype, result);
	//we release the solver now, even if we immediately claim it again in the chaining
	//this has the benefit that we do not need to worry about releasing it in case running doesnt work
	//the downside is that the solver is not claimed for a short period,
	//in which theoretically the main loop can snatch it (which would mean this job here would just be queued)
	solver->releaseScatterjobClaim();
	//immediately chain a next job into the system
	if (_params.scatterChain()) {
		//now that we have a result, it might be that we want to create a special job based on that result
		auto new_job = getNewJobBasedOnResult(reporting_id, jobtype, std::move(result));
		if (isEmpty(new_job)) {
			//nothing special was created, fallback to a generic one
			new_job = getJob(reporting_id, solver->getSolvertype());
		}
		tryToRunJob(std::move(new_job));
	}
}


//// Created by nicco on 11/4/26.
//

#include "scatterengine.hpp"

ScatterEngine::ScatterEngine(std::vector<std::shared_ptr<SolverThread>> &solver_threads, Parameters &params) :
	_params(params),
	_solver_threads(solver_threads)
{

}

void ScatterEngine::init() {
	if (_initialized) return;
	for (auto &map : getMappingDefinitions()) {
		_mapped_jobtypes_to_solvertype.at(map.solvertype).push_back(map.jobtype);
	}
	_initialized = true;
}

bool ScatterEngine::doesJobMapToSolver(Jobtype jobtype, SolverBackendType solvertype) const {
	for (auto mapped_jobtype : _mapped_jobtypes_to_solvertype.at(solvertype)) {
		if (jobtype == mapped_jobtype) {
			return true;
		}
	}
	return false;
}

int ScatterEngine::loop() {
	//if terminated: return //do we need this? safetynet to not access solvers while we are cleaning up...
	for (auto &solver : _solver_threads) {
		if (!solver) continue;
		if (solver->claimForScatterjob()) {
			deployNewJobForSolverOrRelease(solver);
		}
	}
	return 0;
}

void ScatterEngine::deployNewJobForSolverOrRelease(solverptr solver) {
	auto job = getJobForSolver(solver);
	bool deployed = tryDeployJob(std::move(job));
	if (!deployed) {
		solver->releaseScatterjobClaim();
	}
}

ScatterEngine::Scatterjob ScatterEngine::getJobForSolver(solverptr solver) {
	//look first in queue(s) if there exist any job for this solvertype
	assert(solver->isClaimedForScatterJob());
	for (auto jobtype : _mapped_jobtypes_to_solvertype.at(solver->getSolvertype())) {
		auto &queue = _scatterjob_queues.at(jobtype);
		//Almost always the queue will be empty anyways, don't need a mutex for that
		if (!queue.empty()) {
			//only when indeed something is inside we need to care about exclusive access
			std::lock_guard lock(_scatterjob_queues_mutex);
			if (!queue.empty()) {
				auto job = std::move(queue.front()); queue.pop();
				job.preferred_id = solver->getLocalId();
				return job;
			}
		}
	}
	//otherwise create a new job (might be obsolete for some solvertypes which dont support default jobs)
	return createNewDefaultJob(solver);
}

bool ScatterEngine::isJobObsolete(Scatterjob &job) {
	return job.definition.empty() && job.payload.empty();
}

bool ScatterEngine::tryDeployJob(Scatterjob job) {
	if (isJobObsolete(job))
		return false;
	solverptr solver = nullptr;
	if (job.has_claimed_solver)
		solver = _solver_threads[job.preferred_id];
	else
		solver = claimFreeSolverOrNull(job.preferred_id, job.jobtype);
	if (solver) {
		job.running_id = solver->getLocalId();
		assert(job.running_id!=-1);
		instantiateJobForSpecificSolver(job);
		if (isJobObsolete(job)) {
			solver->releaseScatterjobClaim();
			return false;
		}
		solver->appendRevision(job.revision, {std::make_shared<std::vector<int>>(std::move(job.payload)), {}});
		storeJobAsRunning(std::move(job));
		return true;
	} else {
		requeueJob(job);
		return false;
	}

}

solverptr ScatterEngine::claimFreeSolverOrNull(int preferred_id, Jobtype jobtype) {
	const size_t n = _solver_threads.size();
	if (n == 0) return nullptr;
	//loop over all IDs but start with the preferred one
	for (size_t j = 0; j < n; j++) {
		const size_t id = (preferred_id + j) % n;
		const solverptr solver = _solver_threads.at(id);
		if (solver && doesJobMapToSolver(jobtype, solver->getSolvertype()) && solver->claimForScatterjob()) {
				return solver;
		}
	}
	return nullptr;
}


void ScatterEngine::storeJobAsRunning(Scatterjob &&job) {
	//The slot where we want to store the job should be empty (or filled with a dummy job)
	assert( _scatterjobs_running[job.running_id].running_id==-1);
	_scatterjobs_running[job.running_id] = std::move(job);
}

ScatterEngine::Scatterjob &&ScatterEngine::extractStoredRunningJob(int reporting_id) {
	assert(_scatterjobs_running[reporting_id].running_id == reporting_id);
	return std::move(_scatterjobs_running[reporting_id]);
}

void ScatterEngine::requeueJob(Scatterjob &job) {
	job.has_claimed_solver = false;
	std::lock_guard lock(_scatterjob_queues_mutex);
	_scatterjob_queues.at(job.jobtype).push(job);
}

void ScatterEngine::cbScatterjobResult(int reporting_id, Jobtype jobtype) {
	auto job = std::move(extractStoredRunningJob(reporting_id));
	auto solver = _solver_threads[reporting_id];
	assert(solver->hasFoundResult(job.revision));
	auto result = std::move(solver->getSatResult());
	readScatterjobResult(reporting_id, jobtype, result); //engine-specific
	tryChainNextJob(reporting_id, jobtype, std::move(result)); //handles keeping/releasing the claim
}


void ScatterEngine::tryChainNextJob(int reporting_id, Jobtype jobtype, JobResult &&result) {
	bool releaseClaimOnReportingSolver = false;
	auto job = getSpecialChainedJobOrDummy(reporting_id, jobtype, std::move(result));
	if (!job.definition.empty() || !job.payload.empty()) {
		//its a proper job
		tryDeployJob(std::move(job));
		releaseClaimOnReportingSolver = true;
	}
	if (job.dummy) {
		//there is no special job, so we try to just
		deployNewJobForSolverOrRelease(_solver_threads[reporting_id]);
	}
	else {

	}





}

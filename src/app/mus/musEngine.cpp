// 
//  Created by nicco on 11/5/26.
// 

#include "musEngine.hpp"


MusEngine::MusEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface)
	: ScatterEngine(params, config, loggingInterface)
{
	ScatterEngine::init();
	_numThreads = config.threads; // allocated "physical" threads
	_solverdata.resize(_num_solvers); // allocated solvers (of which not all run at the same time)
	// numClauses, firstAssumpVar, ...
}


std::string MusEngine::getSolverChoicesStr() {
	// The MUS engine uses incremental Cadical and Rotator solvers, allocated in a 1:1 ratio.
	// Uppercase C signals that we want Cadical incremental.
	return "Cr";
}
int MusEngine::getNumSolvers(int numThreads) {
	// For each available physical thread/core we allocate two solvers (Cadical and Rotator, see the "Cr" string)
	return 2 * numThreads;
}

std::vector<ScatterEngine::JobToSolverMapping> MusEngine::getJobToSolverMappingDefinitions() {
	return {
		{KICK_CANDIDATE_CLAUSE, SolverBackendType::CADICAL},
		{ROTATE_MODEL_AROUND_CLAUSE, SolverBackendType::ROTATOR}
	};
}

int MusEngine::posToVar(int pos) {
	return pos + _firstAssumpVar;
}

int MusEngine::varToPos(int var) {
	return std::abs(var) - _firstAssumpVar;
}

void MusEngine::setCandidateAssumpVar(Scatterjob &job, int a) {
	assert(job.jobtype == KICK_CANDIDATE_CLAUSE);
	assert(job.definition.empty());
	job.definition.push_back(a);
}

bool MusEngine::hasCandidateAssumpVar(const Scatterjob &job) {
	assert(job.jobtype == KICK_CANDIDATE_CLAUSE);
	return job.definition.size()==1;
}

int MusEngine::getCandidateAssumpVar(const Scatterjob &job) {
	assert(hasCandidateAssumpVar(job));
	return job.definition.back();
}

MusEngine::Scatterjob MusEngine::createNewDefaultJob(int preferred_id, SolverBackendType solvertype) {
	auto job = Scatterjob();
	// Only for Cadical-type solvers we can create default jobs.
	// All other solvers get an empty dummy job
	if (solvertype != SolverBackendType::CADICAL) {
		return job;
	}
	assert(solvertype == SolverBackendType::CADICAL);
	// The only default job is the one to kick the next undecided variable
	job.preferred_id = preferred_id;
	job.jobtype = KICK_CANDIDATE_CLAUSE;
	int a_cand = getNextCandidateVar();
	if (!a_cand) {
		// nothing to kick anymore, return empty
		return job;
	}
	// a proper job
	setCandidateAssumpVar(job, a_cand);
	return job;
}

void MusEngine::instantiateJob(Scatterjob &job) {
	assert(job.running_id!=-1);
	assert(!job.definition.empty());
	assert(job.payload.empty());
	if (job.jobtype == KICK_CANDIDATE_CLAUSE) {
		// Collect all newly imported fixed units from the engine, which the solver doesn't know yet.
		std::vector<int> Afixed_new{};
		const int running_id = job.running_id;
		auto &solverdata = _solverdata[running_id];
		const int a_cand = getCandidateAssumpVar(job);
		{
			auto lock  = _Mutex_status_fixed.getSharedLock();
			const int first_unread_pos = solverdata.Afixed_first_unread_pos;
			for (int pos = first_unread_pos ; pos < _Afixed.size() ; pos++ ) {
				const int a_fixed = _Afixed[pos];
				if (std::abs(a_fixed) == a_cand) {
					// edgecase: job has already been solved by somebody else in the meantime
					// don't write anything into this solver (since we don't commit to the job),
					// only mark the job empty and exit.
					job.definition.clear();
					return;
				}
				Afixed_new.push_back(a_fixed);
			}
			//we commit to this job, so now we can progress the solver data accordingly
			solverdata.Afixed_first_unread_pos = _Afixed.size();
		}
		// Store a copy of those fixed units for this solver,
		// to later know the exact initial conditions of this solving call.
		for (int a_fixed : Afixed_new) {
			const int pos = varToPos(a_fixed);
			assert(solverdata.Astatus[pos]==0);
			solverdata.Astatus[pos] = sign(a_fixed);
		}
		// Pass all new fixed assumptions to the solver by encoding them in this revision as unit clauses
		for (int a_fixed : Afixed_new) {
			job.payload.push_back(a_fixed);
			job.payload.push_back(0);
		}
		// Mark separator between clauses and assumptions
		job.payload.push_back(INT32_MAX);
		// Deactivate the specific candidate clause by satisfying its guarding literal (-a).
		// reduce all other clauses to their original form by unsatisfiying their guarding literal.
		for (int pos = 0; pos < _numAssumptions; pos++) {
			if (solverdata.Astatus[pos]==0) {
				const int a = posToVar(pos);
				if (a == a_cand)
					job.payload.push_back(-a); // deactivate
				else
					job.payload.push_back(a); // reduce to original
			}
		}
		// Mark end of assumptions
		job.payload.push_back(INT32_MIN);
		// job is now fully instantiated
	}
	if (job.jobtype == ROTATE_MODEL_AROUND_CLAUSE) {
		// todo...
	}
}


int MusEngine::getNextCandidateVar() {
	// Walk along the status of every candidate and get the first that is still undecided
	// Implicitly some workbalancing between processes happens here, because every process has
	// a different (and often rebalanced) starting index
	if (_noCandidatesLeft)
		return 0;
	auto lock  = _Mutex_status_fixed.getSharedLock();
	auto lock2 = _Mutex_pos.getUniqueLock();
	int pos = _Astatus_next_candidate_pos;
	int detect_roundtrip = pos;
	while (_Astatus[pos] != 0) {
		pos++;
		if (pos >= _Astatus.size()) // wrap to start
			pos=0;
		if (pos == detect_roundtrip) {
			 // we did one full roundtrip without candidates, exit
			_noCandidatesLeft = true;
			return 0;
		}
	}
	_Astatus_next_candidate_pos = (pos + 1) % _Astatus.size();
	int a_candidate = posToVar(pos);
	return a_candidate;
}


char MusEngine::sign(int a) {
	assert(a!=0);
	return a > 0 ? 1 : -1;
}

void MusEngine::newCoreFoundLocally(std::vector<int> &&core) {
	int new_size = getCoreSize(core);
	bool commitToNewCore = false;
	{
		//TODO: reject a core if it contains a variable that in the meantime has already been deactivated globally
		//TODO: make core handling more robust against interweaving threads
		auto lock = _Mutex_found_locally.getUniqueLock();
		int old_size = getCoreSize(_Acore_found_locally);
		// possible optimization: keep live track of the stored core size instead of recomputing it here each time...
		if (new_size < old_size) {
			LOG(V2_INFO, "Commited new core size %i / %i", new_size, core.size());
			_Acore_found_locally = std::move(core);
			commitToNewCore = true;
		}
	}
	if (commitToNewCore && _params.musNonsharingEngine()) {
		//claims its own mutexes
		fixVariablesOutsideCore_MOVE_TO_CLIENT_LATER();
	}
}

// all variables outside the core which are still undecided can be fixed (to the state unnecessary)
void MusEngine::fixVariablesOutsideCore_MOVE_TO_CLIENT_LATER() {
	// its easier to find the variables NOT in the core by blasting it into a bool array
	std::vector<bool> isInCore = std::vector<bool>(_numAssumptions, false);
	{
		auto lock = _Mutex_found_locally.getSharedLock();
		for (int a : _Acore_found_locally) {
			isInCore[varToPos(a)]=true;
		}
	}
	std::vector<int> Afixed{};
	{
		auto lock = _Mutex_status_fixed.getSharedLock();
		for (int pos = 0; pos < _numAssumptions; pos++) {
			// can be deactivated if not in the core and still undecided
			if (!isInCore[pos] && _Astatus[pos]==0) {
				int a_fixed = - posToVar(pos); // MINUS !! the variable is unnecessary
				Afixed.push_back(a_fixed);
			}
		}
	}
	newGloballyFixedAssumptions(std::move(Afixed));
}

void MusEngine::newNessAssumpFoundLocally(int a) {
	// this local call can only happen for necessary assumptions (positive)
	// because unnecessary ones (negative) would lead to an Unsat Core instead
	assert(a>0);
	{
		auto lock = _Mutex_found_locally.getUniqueLock();
		_Aness_found_locally.push_back(a);
	}
	if (_params.musNonsharingEngine()) {
		newGloballyFixedAssumptions({a});
	}
}

void MusEngine::newGloballyFixedAssumptions(std::vector<int> &&Afixed) {
	{
		auto lock  =  _Mutex_status_fixed.getUniqueLock();
		for (int a : Afixed) {
			int pos = varToPos(a);
			if (_Astatus[pos]!=0) {
				// already fixed by another thread, ignore
				assert(_Astatus[pos] == sign(a));
				continue;
			}
			_Astatus[pos] = sign(a);
			_Afixed.push_back(a);
		}
	}
	interruptOutdatedJobs(std::move(Afixed));
}

//Maybe move this functionality more generally to ScatterEngine later
void MusEngine::interruptOutdatedJobs(std::vector<int> &&Afixed) {
	auto lock = _mutex_scatterjobs_running.getLock();
	for (auto &running_job : _scatterjobs_stored_running) {
		if (isEmpty(running_job))
			continue;
		if (running_job.jobtype != KICK_CANDIDATE_CLAUSE)
			continue;
		assert(hasCandidateAssumpVar(running_job));
		for (int a_fixed : Afixed) {
			if (getCandidateAssumpVar(running_job) == std::abs(a_fixed)) {
				// job is outdated, its running solver should be terminated
				auto solver = _solver_threads[running_job.running_id];
				assert(solver->isClaimedForScatterJob());
				solver->interruptRevision(running_job.revision);
			}
		}
	}
}


int MusEngine::getCoreSize(const std::vector<int> &core) {
	if (core.size()==0)  // doesnt exist yet, so we want to replace it
		return INT32_MAX;
	int size = 0;
	auto lock = _Mutex_status_fixed.getSharedLock();
	for (int a : core) {
		size += _Astatus[varToPos(a)]==0;
	}
	return size;
}


void MusEngine::readScatterjobResult(int reporting_id, Jobtype jobtype, const JobResult &result, const Scatterjob &job) {
	if (jobtype == KICK_CANDIDATE_CLAUSE) {
		if (result.result == SAT) {
			int a_ness = getCandidateAssumpVar(job);
			newNessAssumpFoundLocally(a_ness);
			// the model in the solution vector is not handled here, but in chaining a next job
		} else if (result.result == UNSAT) {
			// the ScatterEngine only allows us to read the result here, not extract/modify it
			auto failedAssumptions = result.copySolution();
			newCoreFoundLocally(std::move(failedAssumptions));
		} else {
			// job didn't finish
			assert(result.result == UNKNOWN);
		}
	}
	if (jobtype == ROTATE_MODEL_AROUND_CLAUSE) {
		// todo... read rotation result
	}
}

ScatterEngine::Scatterjob MusEngine::createNewJobFromResult(int reporting_id, Jobtype old_jobtype, JobResult &&old_result, const Scatterjob &old_job) {
	if (old_jobtype == KICK_CANDIDATE_CLAUSE && old_result.result == SAT && _params.musRotation()) {
		// A SAT call where one clause has been kicked came back as satisfiable.
		// This means that this clause was necessary, and also that we got a model.
		// Now we can rotate this model a bit to maybe stumble upon nearby other necessary clauses.
		int a_ness = getCandidateAssumpVar(old_job);
		auto model = old_result.extractSolution();
		auto rotation_job = createNewRotationJob(a_ness, std::move(model));
		// This new job will *not* run on the chaining Cadical solver but on Rotator solver
		// In principle the deployment process will find such a solver on its own,
		// but it doesnt harm if we softly prefer the "neighbouring" rotation id.
		rotation_job.preferred_id = reporting_id + 1;
		return rotation_job;
	}
	return Scatterjob(); // empty fallback
}

ScatterEngine::Scatterjob MusEngine::createNewRotationJob(int a_ness, std::vector<int> &&model) {
	// todo: create rotation job
	return Scatterjob();
}

void MusEngine::rebalanceWork() {
	// We do work partitioning by providing each solver a different point in the Astatus array
	// from which it starts to pick its work (candidate assumptions). This way every solver
	// works on its own range of candidate assumptions, and the work is partitioned.
	// However, some solvers will progress faster trough their allotted range than others, so a simple static partition
	// is not ideal. Instead, we rebalance periodically such that they all keep the same number of yet-undecided assumptions
	// ahead of themselves before the range of the next worker begins.
	// Non-Malleable for now: Reading the static initial _config values, which don't (?) get updated on malleable job resizing...
	const int myindex = _config.apprank; // called rank, but is the index.
	const int workers = _config.mpisize;
	assert(workers>0);
	assert(myindex < workers);
	int numFixedAssumps = 0;
	{
		auto lock = _Mutex_status_fixed.getSharedLock();
		numFixedAssumps = _Afixed.size();
	}
	const int numUndecidedAssumps = _numAssumptions - numFixedAssumps;
	const double myStartFraction = (double)myindex / (double)workers;
	const int myCountToStart = myStartFraction * numUndecidedAssumps;
	// We now know how many undecided assumptions are left within the whole array.
	// And we know at which fraction and thus at which count of undecided assumptions our range should start.
	// Scan the array and find that starting position.
	int myPosToStart = 0;
	{
		auto lock = _Mutex_status_fixed.getSharedLock();
		int undecided = 0;
		const int size = _Astatus.size();
		for (int i = 0; i < size; i++) {
			undecided += _Astatus[i]==0;
			if (undecided == myCountToStart) {
				myPosToStart = i;
				break;
			}
		}
	}
	auto lock = _Mutex_pos.getUniqueLock();
	_Astatus_next_candidate_pos = myPosToStart;
}

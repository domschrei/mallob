// 
//  Created by nicco on 11/5/26.
// 

#include "musEngine.hpp"


MusEngine::MusEngine(const Parameters& params, const SatProcessConfig& config, Logger& loggingInterface)
	: ScatterEngine(params, config, loggingInterface)
{
	// numClauses, firstAssumpVar, ...
}

void MusEngine::init() {
	ScatterEngine::init();
	// allocate some bookkeeping for each individual solver
	_solverdata.resize(_num_solvers);
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
		assert(isEmpty(job));
		return job;
	}
	// a proper job
	setCandidateAssumpVar(job, a_cand);
	assert(!isEmpty(job));
	return job;
}

void MusEngine::instantiateJob(Scatterjob &job) {
	assert(job.running_id!=-1); //We know the running id
	assert(!job.definition.empty()); //The job needs to be defined
	assert(job.payload.empty()); //It should not yet be instantiated
	if (job.jobtype == KICK_CANDIDATE_CLAUSE) {
		// Collect all newly imported fixed units from the engine, which the solver doesn't know yet.
		std::vector<int> Afixed_new{};
		const int running_id = job.running_id;
		auto &solverdata = _solverdata[running_id];
		const int a_cand = getCandidateAssumpVar(job);
		{
			auto lock  = _Mutex_status_fixed.getSharedLock();
			if (_Astatus[varToPos(a_cand)]!=0) {
				// edgecase, this candidate variable has already been fixed in the meantime
				// so this job became obsolete
				job.definition.clear();
				assert(isEmpty(job));
				return;
			}
			// Candidate is still open, so now we committed to this job
			// Update the solver about the latest fixes that it doesn't know about yet
			// and update its own counter then
			assert(_Astatus[varToPos(a_cand)]==0);
			if (solverdata.Afixed_first_unread_pos < (int)_Afixed.size()) {
				Afixed_new.insert(Afixed_new.end(), _Afixed.begin() + solverdata.Afixed_first_unread_pos, _Afixed.end());
				solverdata.Afixed_first_unread_pos = _Afixed.size();
			}
		}
		// Store a copy of the fixes for this solver in particular,
		// necessary such that later we can know the exact initial starting state of this solving call
		for (int a_fixed : Afixed_new) {
			const int pos = varToPos(a_fixed);
			assert(solverdata.Astatus[pos]==0);
			solverdata.Astatus[pos] = sign(a_fixed);
		}
		// Pass all new fixed assumptions to the solver by encoding them as unit clauses
		for (int a_fixed : Afixed_new) {
			job.payload.push_back(a_fixed);
			job.payload.push_back(0);
		}
		// separator between clauses and assumptions
		job.payload.push_back(INT32_MAX);
		// Deactivate the specific candidate clause by satisfying its guarding literal -a.
		// Reduce all other clauses to their original form by unsatisfying their guard literal.
		for (int pos = 0; pos < _numAssumptions; pos++) {
			if (solverdata.Astatus[pos]==0) {
				const int a = posToVar(pos);
				if (a == a_cand)
					job.payload.push_back(-a); // deactivate
				else
					job.payload.push_back(a); // reduce to original
			}
		}
		// separator for end of assumptions
		job.payload.push_back(INT32_MIN);
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
	//Need exclusive access here to make sure that each new core is handled cleanly on its own
	bool commitToNewCore = false;
	{
		auto lock1 = _Mutex_status_fixed.getUniqueLock();
		auto lock2 = _Mutex_found_locally.getUniqueLock();
		int size_new = getCoreUndecidedSizeLocked(core);
		int size_old = getCoreUndecidedSizeLocked(_Acore_found_locally);
		if (size_new == INT32_MAX && size_old == INT32_MAX) {
			//Weird edgecase that both cores are invalid. Can this ever happen?
			LOG(V1_WARN, "BOTH_CORES_INVALID!\n");
			return;
		}
		if (size_new < size_old) {
			LOG(V2_INFO, "Commited to new core of effective size %i / %i", size_new, core.size());
			_Acore_found_locally = std::move(core);
			// commitToNewCore = true;
		}
	}

	// if (commitToNewCore && _params.musNonsharingEngine()) {
		// claims its own mutexes
		// fixVariablesOutsideCore_MOVE_TO_CLIENT_LATER();
	// }
}

void MusEngine::fixVariablesOutsideCore_MOVE_TO_CLIENT_LATER() {
	// all variables outside the core which are still undecided can be fixed to be unnecessary

	// To find those variables, we blast the core into a flat boolean array
	// and then walk through it and note every slot that is NOT filled.
	std::vector<bool> isInCore = std::vector<bool>(_numAssumptions, false);
	{
		auto lock = _Mutex_found_locally.getSharedLock();
		for (int a : _Acore_found_locally) {
			isInCore[varToPos(a)]=true;
		}
	}

	std::vector<int> AoutsideCore{};
	{
		auto lock = _Mutex_status_fixed.getSharedLock();
		for (int pos = 0; pos < _numAssumptions; pos++) {
			// can be deactivated if not in the core and still undecided
			if (!isInCore[pos] && _Astatus[pos]==0) {
				// MINUS !! marks the variable as unnecessary
				int a_fixed_unness = - posToVar(pos);
				assert(a_fixed_unness < 0);
				AoutsideCore.push_back(a_fixed_unness);
			}
		}
	}
	newGloballyFixedAssumptions(std::move(AoutsideCore));
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


//Get the number of still undecided variables within this UNSAT core
int MusEngine::getCoreUndecidedSizeLocked(const std::vector<int> &core) {
	//_Astatus mutex needs to be locked!
	if (core.size()==0) {
		// only the initial empty core can trigger this, so we give a large fake number to make sure
		// it is replaced by the other compared core
		return INT32_MAX;
	}
	int size = 0;
	for (int a : core) {
		const int status = _Astatus[varToPos(a)];
		size += status==0;
		if (a > 0 && status < 0) {
			// Edgecase: The core became invalid. Invalidate it against its comparison by giving it a huge size.
			LOG(V1_WARN, "WARN Core contained positive assumption that in the meantime has been marked useless!\n");
			return INT32_MAX;
		}
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


/* export data */ std::vector<int> MusEngine::scattershareExport(std::vector<int> &&request_for_export) {
	//read and send our Aness and Core back
	return {};
}

/* feedback on import */ std::vector<int> MusEngine::scattershareImport(std::vector<int> &&data_to_import) {
	//import and maybe send some feedback
	return {};
}


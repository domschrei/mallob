
#include <thread>
#include "util/assert.hpp"

#include "threaded_sat_job.hpp"

#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "anytime_sat_clause_communicator.hpp"
#include "util/sys/proc.hpp"
#include "../execution/engine.hpp"
#include "sat_process_config.hpp"
#include "util/sys/thread_pool.hpp"

ThreadedSatJob::ThreadedSatJob(const Parameters& params, const JobSetup& setup, AppMessageTable& table) : 
        BaseSatJob(params, setup, table), _done_locally(false) {}

void ThreadedSatJob::appl_start() {

    assert(!_initialized);
    
    // Initialize SAT engine
    Parameters hParams(_params);
    hParams.applicationConfiguration.set(getDescription().getAppConfiguration().serialize());
    SatProcessConfig config(_params, *this, /*recoveryIndex=*/0);
    _solver = std::unique_ptr<SatEngine>(
        new SatEngine(hParams, config, Logger::getMainInstance())
    );
    _clause_comm.reset(new AnytimeSatClauseCommunicator(_params, this));

    //log(V5_DEBG, "%s : beginning to solve\n", toStr());
    const JobDescription& desc = getDescription();
    while (_last_imported_revision < desc.getRevision()) {
        _last_imported_revision++;
        _solver->appendRevision(_last_imported_revision, 
            desc.getFormulaPayloadSize(_last_imported_revision), 
            desc.getFormulaPayload(_last_imported_revision), 
            desc.getAssumptionsSize(_last_imported_revision), 
            desc.getAssumptionsPayload(_last_imported_revision)
        );
    }
    _solver->solve();
    //log(V4_VVER, "%s : finished SAT engine initialization\n", toStr());
    _time_of_start_solving = Timer::elapsedSeconds();
    _initialized = true;
}

void ThreadedSatJob::appl_suspend() {
    if (!_initialized) return;
    getSolver()->setPaused();
    _clause_comm->communicate();
}

void ThreadedSatJob::appl_resume() {
    if (!_initialized) return;
    getSolver()->unsetPaused();
}

void ThreadedSatJob::appl_terminate() {
    if (!_initialized) return;
    getSolver()->terminateSolvers();
}

JobResult&& ThreadedSatJob::appl_getResult() {
    _result = std::move(getSolver()->getResult());
    _result.id = getId();
    _result.updateSerialization();
    assert(_result.revision == getRevision());
    return std::move(_result);
}

int ThreadedSatJob::appl_solved() {

    int result = -1;

    // Import new revisions as necessary
    {
        const JobDescription& desc = getDescription();
        while (_last_imported_revision < desc.getRevision()) {
            _last_imported_revision++;
            _solver->appendRevision(
                _last_imported_revision,
                desc.getFormulaPayloadSize(_last_imported_revision), 
                desc.getFormulaPayload(_last_imported_revision),
                desc.getAssumptionsSize(_last_imported_revision),
                desc.getAssumptionsPayload(_last_imported_revision)
            );
            _done_locally = false;
            _result = JobResult();
            _result_code = 0;
        }
    }

    // Already reported the actual result, or still initializing
    if (_done_locally || !_initialized || getState() != ACTIVE) {
        return result;
    }

    result = getSolver()->solveLoop();

    // Did a solver find a result?
    if (result >= 0) {
        _done_locally = true;
        LOG_ADD_DEST(V2_INFO, "%s : found result %s", getJobTree().getRootNodeRank(), toStr(), 
                            result == RESULT_SAT ? "SAT" : result == RESULT_UNSAT ? "UNSAT" : "UNKNOWN");
        _result_code = result;
    }
    return result;
}

void ThreadedSatJob::appl_dumpStats() {

    if (!_initialized || getState() != ACTIVE) return;

    getSolver()->dumpStats(/*final=*/false);
    if (_time_of_start_solving <= 0) return;
    
    std::vector<long> threadTids = getSolver()->getSolverTids();
    for (size_t i = 0; i < threadTids.size(); i++) {
        if (threadTids[i] < 0) continue;
        double cpuRatio; float sysShare;
        bool ok = Proc::getThreadCpuRatio(threadTids[i], cpuRatio, sysShare);
        if (ok) LOG(V3_VERB, "%s td.%ld cpuratio=%.3f sys=%.3f\n", 
                toStr(), threadTids[i], cpuRatio, 100*sysShare);
    }
}

bool ThreadedSatJob::appl_isDestructible() {
    if (!_initialized) return true;
    if (!_clause_comm->isDestructible()) {
        _clause_comm->communicate(); // may advance destructibility
        return false;
    }
    // Destructible!
    if (!_destroy_future.valid()) {
        _destroy_future = ProcessWideThreadPool::get().addTask([this]() {
            _solver->cleanUp();
        });
        return false;
    }
    return _destroy_future.valid() && _solver->isCleanedUp();
}

void ThreadedSatJob::appl_communicate() {
    if (!_initialized) return;
    _clause_comm->communicate();
}

void ThreadedSatJob::appl_communicate(int source, int mpiTag, JobMessage& msg) {
    if (!_initialized) {
        msg.returnToSender(source, mpiTag);
        return;
    }
    _clause_comm->handle(source, mpiTag, msg);
}

void ThreadedSatJob::appl_memoryPanic() {
    // TODO
}

bool ThreadedSatJob::isInitialized() {
    if (!_initialized) return false;
    return _solver->isFullyInitialized();
}
void ThreadedSatJob::prepareSharing() {
    // Already prepared sharing?
    if (!_clause_buffer.empty()) return;
    
    _clause_buffer.resize(2*_clsbuf_export_limit+100);
    _clause_checksum = Checksum();
    int actualSize = _solver->prepareSharing(_clause_buffer.data(), _clsbuf_export_limit, 
        _successful_solver_id, _num_collected_lits);
    _clause_buffer.resize(actualSize);
}
bool ThreadedSatJob::hasPreparedSharing() {
    return !_clause_buffer.empty();
}
std::vector<int> ThreadedSatJob::getPreparedClauses(Checksum& checksum, int& successfulSolverId, int& numLits) {
    std::vector<int> out = std::move(_clause_buffer);
    _clause_buffer.clear();
    checksum = _clause_checksum;
    successfulSolverId = _successful_solver_id;
    numLits = _num_collected_lits;
    return out;
}
int ThreadedSatJob::getLastAdmittedNumLits() {
    return _solver->getLastAdmittedClauseShare().nbAdmittedLits;
}
void ThreadedSatJob::setClauseBufferRevision(int revision) {
    _solver->setClauseBufferRevision(revision);
}

void ThreadedSatJob::filterSharing(int epoch, std::vector<int>& clauses) {
    auto maxFilterSize = clauses.size()/(8*sizeof(int))+1;
    if (_filter.size() < maxFilterSize) _filter.resize(maxFilterSize);
    int filterSize = _solver->filterSharing(clauses.data(), clauses.size(), _filter.data());
    _filter.resize(filterSize);
    _clauses_to_filter = clauses;
    _did_filter = true;
}
bool ThreadedSatJob::hasFilteredSharing(int epoch) {
    return _did_filter;
}
std::vector<int> ThreadedSatJob::getLocalFilter(int epoch) {
    std::vector<int> filter = std::move(_filter);
    _filter.clear();
    _did_filter = false;
    return filter;
}
void ThreadedSatJob::applyFilter(int epoch, std::vector<int>& filter) {
    _solver->digestSharingWithFilter(_clauses_to_filter.data(), _clauses_to_filter.size(), filter.data());
}

void ThreadedSatJob::digestSharingWithoutFilter(std::vector<int>& clauses) {
    _solver->digestSharingWithoutFilter(clauses.data(), clauses.size());
    if (getJobTree().isRoot()) {
        LOG(V3_VERB, "%s : Digested clause buffer of size %ld\n", toStr(), clauses.size());
    }
}

void ThreadedSatJob::returnClauses(std::vector<int>& clauses) {
    _solver->returnClauses(clauses.data(), clauses.size());
}

void ThreadedSatJob::digestHistoricClauses(int epochBegin, int epochEnd, std::vector<int>& clauses) {
    _solver->digestHistoricClauses(epochBegin, epochEnd, clauses.data(), clauses.size());
}

ThreadedSatJob::~ThreadedSatJob() {
    if (!_initialized) return;
    LOG(V5_DEBG, "%s : enter TSJ destructor\n", toStr());
    _destroy_future.get();
    LOG(V5_DEBG, "%s : destructed TSJ\n", toStr());
}
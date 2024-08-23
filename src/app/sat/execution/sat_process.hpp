
#pragma once

#include <fstream>
#include <iostream>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <memory>
#include "app/sat/data/clause_metadata.hpp"
#include "app/sat/job/inplace_sharing_aggregation.hpp"
#include "util/assert.hpp"

#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "data/checksum.hpp"
#include "util/sys/terminator.hpp"
#include "util/sys/bidirectional_pipe.hpp"
#include "clause_pipe_defs.hpp"

#include "engine.hpp"
#include "../job/sat_shared_memory.hpp"

class SatProcess {

private:
    const Parameters& _params;
    const SatProcessConfig& _config;
    Logger& _log;

    std::string _shmem_id;
    SatSharedMemory* _hsm;

    int _last_imported_revision;
    int _last_present_revision;
    int _desired_revision;
    Checksum* _checksum;

    std::vector<std::vector<int>> _read_formulae;
    std::vector<std::vector<int>> _read_assumptions;

public:
    SatProcess(const Parameters& params, const SatProcessConfig& config, Logger& log) 
        : _params(params), _config(config), _log(log) {

        // Set up "management" block of shared memory created by the parent
        _shmem_id = _config.getSharedMemId(Proc::getParentPid());
        LOGGER(log, V4_VVER, "Access base shmem: %s\n", _shmem_id.c_str());
        _hsm = (SatSharedMemory*) accessMemory(_shmem_id, sizeof(SatSharedMemory));
        
        _checksum = params.useChecksums() ? new Checksum() : nullptr;

        // Adjust OOM killer score to make this process the first to be killed
        // (always better than touching an MPI process, which would crash everything)
        std::ofstream oomOfs("/proc/self/oom_score_adj");
        oomOfs << "1000";
        oomOfs.close();
    }

    void run() {
        SatEngine engine(_params, _config, _log);
        try {
            mainProgram(engine); // does not return
        } catch (const std::exception& ex) {
            LOG(V0_CRIT, "[ERROR] uncaught \"%s\"\n", ex.what());
            Process::doExit(1);
        } catch (...) {
            LOG(V0_CRIT, "[ERROR] uncaught exception\n");
            Process::doExit(1);
        }
    }

private:
    void doImportClauses(SatEngine& engine, std::vector<int>& incomingClauses, std::vector<int>* filterOrNull, int revision, int epoch) {
        LOGGER(_log, V5_DEBG, "DO import clauses rev=%i\n", revision);
        // Write imported clauses from shared memory into vector
        if (revision >= 0) engine.setClauseBufferRevision(revision);
        if (filterOrNull) {
            engine.digestSharingWithFilter(incomingClauses, *filterOrNull);
        } else {
            engine.digestSharingWithoutFilter(incomingClauses);
        }
        engine.addSharingEpoch(epoch);
        engine.syncDeterministicSolvingAndCheckForLocalWinner();
        _hsm->lastAdmittedStats = engine.getLastAdmittedClauseShare();
    }

    int popLast(std::vector<int>& v) {
        int res = v.back();
        v.pop_back();
        return res;
    }

    void mainProgram(SatEngine& engine) {

        // Set up pipe communication for clause sharing
        BiDirectionalPipe pipe(BiDirectionalPipe::ACCESS,
            TmpDir::get()+_shmem_id+".fromsub.pipe",
            TmpDir::get()+_shmem_id+".tosub.pipe");
        pipe.open();
        LOGGER(_log, V4_VVER, "Pipes set up\n");

        // Wait until everything is prepared for the solver to begin
        while (!_hsm->doBegin) doSleep();
        
        // Terminate directly?
        if (checkTerminate(engine, false)) return;

        // Import first revision
        _desired_revision = _config.firstrev;
        readFormulaAndAssumptionsFromSharedMem(engine, 0);
        _last_imported_revision = 0;
        _last_present_revision = 0;
        // Import subsequent revisions
        importRevisions(engine);
        if (checkTerminate(engine, false)) return;
        
        // Start solver threads
        engine.solve();
        
        std::vector<int> solutionVec;
        std::string solutionShmemId = "";
        char* solutionShmem;
        int solutionShmemSize = 0;
        int lastSolvedRevision = -1;

        int exitStatus = 0;

        bool collectClauses = false;
        int exportLiteralLimit;
        std::vector<int> incomingClauses;

        // Main loop
        while (true) {

            doSleep();
            Timer::cacheElapsedSeconds();

            // Terminate
            if (_hsm->doTerminate || Terminator::isTerminating(/*fromMainThread=*/false)) {
                LOGGER(_log, V4_VVER, "DO terminate\n");
                engine.dumpStats(/*final=*/true);
                break;
            }

            // Read new revisions as necessary
            importRevisions(engine);

            char c;
            while ((c = pipe.pollForData()) != 0) {

                if (c == CLAUSE_PIPE_DUMP_STATS) {
                    LOGGER(_log, V5_DEBG, "DO dump stats\n");
                    pipe.readData(c, true);

                    engine.dumpStats(/*final=*/false);

                    // For this management thread
                    double cpuShare; float sysShare;
                    bool success = Proc::getThreadCpuRatio(Proc::getTid(), cpuShare, sysShare);
                    if (success) {
                        LOGGER(_log, V3_VERB, "child_main cpuratio=%.3f sys=%.3f\n", cpuShare, sysShare);
                    }

                    // For each solver thread
                    std::vector<long> threadTids = engine.getSolverTids();
                    for (size_t i = 0; i < threadTids.size(); i++) {
                        if (threadTids[i] < 0) continue;
                        
                        success = Proc::getThreadCpuRatio(threadTids[i], cpuShare, sysShare);
                        if (success) {
                            LOGGER(_log, V3_VERB, "td.%ld cpuratio=%.3f sys=%.3f\n", threadTids[i], cpuShare, sysShare);
                        }
                    }

                    auto rtInfo = Proc::getRuntimeInfo(Proc::getPid(), Proc::SubprocessMode::FLAT);
                    LOGGER(_log, V3_VERB, "child_mem=%.3fGB\n", 0.001*0.001*rtInfo.residentSetSize);

                } else if (c == CLAUSE_PIPE_PREPARE_CLAUSES) {
                    collectClauses = true;
                    exportLiteralLimit = pipe.readData(c, true)[0];

                } else if (c == CLAUSE_PIPE_FILTER_IMPORT) {
                    incomingClauses = pipe.readData(c, true);
                    int epoch = popLast(incomingClauses);
                    InplaceClauseAggregation agg(incomingClauses);
                    int winningSolverId = agg.successfulSolver();
                    int bufferRevision = agg.maxRevision();
                    agg.stripToRawBuffer();
                    LOGGER(_log, V5_DEBG, "DO filter clauses\n");
                    engine.setClauseBufferRevision(bufferRevision);
                    auto filter = engine.filterSharing(incomingClauses);
                    pipe.writeData(filter, {epoch}, CLAUSE_PIPE_FILTER_IMPORT);
                    if (winningSolverId >= 0) {
                        LOGGER(_log, V4_VVER, "winning solver ID: %i\n", winningSolverId);
                        engine.setWinningSolverId(winningSolverId);
                    }

                } else if (c == CLAUSE_PIPE_DIGEST_IMPORT) {
                    auto filter = pipe.readData(c, true);
                    int epoch = popLast(filter);
                    doImportClauses(engine, incomingClauses, &filter, -1, epoch);
                    pipe.writeData({engine.getLastAdmittedClauseShare().nbAdmittedLits}, CLAUSE_PIPE_DIGEST_IMPORT);

                } else if (c == CLAUSE_PIPE_DIGEST_IMPORT_WITHOUT_FILTER) {
                    incomingClauses = pipe.readData(c, true);
                    int epoch = popLast(incomingClauses);
                    InplaceClauseAggregation agg(incomingClauses);
                    int bufferRevision = agg.maxRevision();
                    agg.stripToRawBuffer();
                    doImportClauses(engine, incomingClauses, nullptr, bufferRevision, epoch);

                } else if (c == CLAUSE_PIPE_RETURN_CLAUSES) {
                    LOGGER(_log, V5_DEBG, "DO return clauses\n");
                    auto clauses = pipe.readData(c, true);
                    int bufferRevision = popLast(clauses);
                    engine.setClauseBufferRevision(bufferRevision);
                    engine.returnClauses(clauses);

                } else if (c == CLAUSE_PIPE_DIGEST_HISTORIC) {
                    LOGGER(_log, V5_DEBG, "DO digest historic clauses\n");
                    auto data = pipe.readData(c, true);
                    int bufferRevision = popLast(data);
                    int epochEnd = popLast(data);
                    int epochBegin = popLast(data);
                    engine.setClauseBufferRevision(bufferRevision);
                    engine.digestHistoricClauses(epochBegin, epochEnd, data);

                } else if (c == CLAUSE_PIPE_REDUCE_THREAD_COUNT) {
                    LOGGER(_log, V3_VERB, "DO reduce thread count\n");
                    pipe.readData(c, true);
                    engine.reduceActiveThreadCount();

                } else if (c == CLAUSE_PIPE_START_NEXT_REVISION) {
                    LOGGER(_log, V5_DEBG, "DO start next revision\n");
                    auto data = pipe.readData(c, true);
                    _last_present_revision = popLast(data);
                    _desired_revision = popLast(data);

                } else {
                    LOGGER(_log, V0_CRIT, "[ERROR] Unknown pipe directive \"%c\"!\n", c);
                    abort();
                }
            }

            // Check initialization state
            if (!_hsm->isInitialized && engine.isFullyInitialized()) {
                LOGGER(_log, V5_DEBG, "DO set initialized\n");
                _hsm->isInitialized = true;
            }
            
            // Terminate "improperly" in order to be restarted automatically
            if (_hsm->doCrash) {
                LOGGER(_log, V3_VERB, "Restarting this subprocess\n");
                exitStatus = SIGUSR2;
                break;
            }

            // Collect clauses if ready for sharing
            if (collectClauses && engine.isReadyToPrepareSharing()) {
                LOGGER(_log, V5_DEBG, "DO export clauses\n");
                // Collect local clauses, put into shared memory
                int successfulSolverId = -1;
                int numCollectedLits;
                auto clauses = engine.prepareSharing(exportLiteralLimit, successfulSolverId, numCollectedLits);
                if (!clauses.empty()) {
                    pipe.writeData(clauses, {numCollectedLits, successfulSolverId}, CLAUSE_PIPE_PREPARE_CLAUSES);
                }
                collectClauses = false;
            }

            // Do not check solved state if the current 
            // revision has already been solved
            if (lastSolvedRevision == _last_imported_revision) continue;

            // Check solved state
            int resultCode = engine.solveLoop();
            if (resultCode >= 0 && !_hsm->hasSolution) {
                // Solution found!
                auto& result = engine.getResult();
                result.id = _config.jobid;
                if (_hsm->doTerminate || result.revision < _desired_revision) {
                    // Result obsolete
                    continue;
                }
                assert(result.revision == _last_imported_revision);

                solutionVec = result.extractSolution();
                _hsm->solutionRevision = result.revision;
                _hsm->winningInstance = result.winningInstanceId;
                _hsm->globalStartOfSuccessEpoch = result.globalStartOfSuccessEpoch;
                LOGGER(_log, V5_DEBG, "DO write solution (winning instance: %i)\n", _hsm->winningInstance);
                _hsm->result = SatResult(result.result);
                size_t* solutionSize = (size_t*) SharedMemory::create(_shmem_id + ".solutionsize." + std::to_string(_hsm->solutionRevision), sizeof(size_t));
                *solutionSize = solutionVec.size();
                // Write solution
                if (*solutionSize > 0) {
                    solutionShmemId = _shmem_id + ".solution." + std::to_string(_hsm->solutionRevision);
                    solutionShmemSize =  *solutionSize*sizeof(int);
                    solutionShmem = (char*) SharedMemory::create(solutionShmemId, solutionShmemSize);
                    memcpy(solutionShmem, solutionVec.data(), solutionShmemSize);
                }
                lastSolvedRevision = result.revision;
                LOGGER(_log, V5_DEBG, "DONE write solution\n");
                _hsm->hasSolution = true;
            }
        }

        Terminator::setTerminating();
        checkTerminate(engine, true, exitStatus); // exits
        abort(); // should be unreachable

        // Shared memory will be cleaned up by the parent process.
    }

    bool checkTerminate(SatEngine& engine, bool force, int exitStatus = 0) {
        bool terminate = _hsm->doTerminate || Terminator::isTerminating(/*fromMainThread=*/true);
        if (terminate && force) {
            // clean up all resources which MUST be cleaned up (e.g., child processes)
            engine.cleanUp(true);
            _log.flush();
            _hsm->didTerminate = true;
            // terminate yourself
            Process::doExit(exitStatus);
        }
        return terminate;
    }

    void readFormulaAndAssumptionsFromSharedMem(SatEngine& engine, int revision) {

        float time = Timer::elapsedSeconds();

        size_t fSize, aSize;
        if (revision == 0) {
            fSize = _hsm->fSize;
            aSize = _hsm->aSize;
        } else {
            size_t* fSizePtr = (size_t*) accessMemory(_shmem_id + ".fsize." + std::to_string(revision), sizeof(size_t));
            size_t* aSizePtr = (size_t*) accessMemory(_shmem_id + ".asize." + std::to_string(revision), sizeof(size_t));
            fSize = *fSizePtr;
            aSize = *aSizePtr;
        }

        const int* fPtr = (const int*) accessMemory(_shmem_id + ".formulae." + std::to_string(revision),
            sizeof(int) * fSize, SharedMemory::READONLY);
        const int* aPtr = (const int*) accessMemory(_shmem_id + ".assumptions." + std::to_string(revision),
            sizeof(int) * aSize, SharedMemory::READONLY);

        if (_params.copyFormulaeFromSharedMem()) {
            // Copy formula and assumptions to your own local memory
            _read_formulae.emplace_back(fPtr, fPtr+fSize);
            _read_assumptions.emplace_back(aPtr, aPtr+aSize);

            // Reference the according positions in local memory when forwarding the data
            engine.appendRevision(revision, fSize, _read_formulae.back().data(),
                aSize, _read_assumptions.back().data(), revision == _desired_revision);
            updateChecksum(_read_formulae.back().data(), fSize);
        } else {
            // Let the solvers read from shared memory directly
            engine.appendRevision(revision, fSize, fPtr, aSize, aPtr, revision == _desired_revision);
            updateChecksum(fPtr, fSize);
        }

        if (revision > 0) {
            // Access checksum from outside
            Checksum* chk = (Checksum*) accessMemory(_shmem_id + ".checksum." + std::to_string(revision), sizeof(Checksum));
            if (chk->count() > 0) {
                // Check checksum
                if (_checksum->get() != chk->get()) {
                    LOGGER(_log, V0_CRIT, "[ERROR] Checksum fail at rev. %i. Incoming count: %ld ; local count: %ld\n", revision, chk->count(), _checksum->count());
                    abort();
                }
            }
        }

        time = Timer::elapsedSeconds() - time;
        LOGGER(_log, V3_VERB, "Read formula rev. %i (size:%lu,%lu) from shared memory in %.4fs\n", revision, fSize, aSize, time);
    }

    void* accessMemory(const std::string& shmemId, size_t size, SharedMemory::AccessMode accessMode = SharedMemory::ARBITRARY) {
        void* ptr = SharedMemory::access(shmemId, size, accessMode);
        if (ptr == nullptr) {
            LOGGER(_log, V0_CRIT, "[ERROR] Could not access shmem %s\n", shmemId.c_str());  
            abort();
        }
        return ptr;
    }

    void updateChecksum(const int* ptr, size_t size) {
        if (_checksum == nullptr) return;
        for (size_t i = 0; i < size; i++) _checksum->combine(ptr[i]);
    }

    void importRevisions(SatEngine& engine) {
        while (_last_imported_revision < _last_present_revision) {
            if (checkTerminate(engine, false)) return;
            _last_imported_revision++;
            readFormulaAndAssumptionsFromSharedMem(engine, _last_imported_revision);
            _hsm->hasSolution = false;
        }
    }

    void doSleep() {
        // Wait until something happens
        // (can be interrupted by Fork::wakeUp(hsm->childPid))
        usleep(1000 /*1 millisecond*/);
    }
};

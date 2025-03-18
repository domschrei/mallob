
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

#include "util/string_utils.hpp"
#include "util/sys/bidirectional_anytime_pipe_shmem.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"
#include "util/params.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/process.hpp"
#include "util/sys/proc.hpp"
#include "data/checksum.hpp"
#include "util/sys/terminator.hpp"
#include "clause_pipe_defs.hpp"

#include "engine.hpp"
#include "../job/sat_shared_memory.hpp"
#include "util/sys/watchdog.hpp"

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

    std::vector<std::vector<int>> _read_formulae;
    std::vector<std::vector<int>> _read_assumptions;

    bool _has_solution {false};

public:
    SatProcess(const Parameters& params, const SatProcessConfig& config, Logger& log) 
        : _params(params), _config(config), _log(log) {

        // Set up "management" block of shared memory created by the parent
        _shmem_id = _config.getSharedMemId(Proc::getParentPid());
        LOGGER(log, V4_VVER, "Access base shmem: %s\n", _shmem_id.c_str());
        _hsm = (SatSharedMemory*) accessMemory(_shmem_id, sizeof(SatSharedMemory));
        _hsm->didStart = true; // signal to parent: I started properly (signal handlers etc.)

        // Adjust OOM killer score to make this process the first to be killed
        // (always better than touching an MPI process, which would crash everything)
        std::ofstream oomOfs("/proc/self/oom_score_adj");
        oomOfs << "150"; // maximum: 1000
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
    void doImportClauses(SatEngine& engine, std::vector<int>& incomingClauses, std::vector<int>* filterOrNull, int revision, int epoch, bool stateless = false) {
        LOGGER(_log, V5_DEBG, "DO import clauses rev=%i\n", revision);
        // Write imported clauses from shared memory into vector
        if (revision >= 0) engine.setClauseBufferRevision(revision);
        if (filterOrNull) {
            engine.digestSharingWithFilter(incomingClauses, *filterOrNull);
        } else {
            engine.digestSharingWithoutFilter(incomingClauses, stateless);
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
        char* pipeParentToChild = (char*) accessMemory(_shmem_id + ".pipe-parenttochild", _hsm->pipeBufSize);
        char* pipeChildToParent = (char*) accessMemory(_shmem_id + ".pipe-childtoparent", _hsm->pipeBufSize);
        BiDirectionalAnytimePipeShmem pipe(
            {pipeChildToParent, _hsm->pipeBufSize, true},
            {pipeParentToChild, _hsm->pipeBufSize, true},
            false);
        LOGGER(_log, V4_VVER, "Pipes set up\n");

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
        
        int lastSolvedRevision = -1;

        int exitStatus = 0;

        bool collectClauses = false;
        int exportLiteralLimit;
        std::vector<int> incomingClauses;

        Watchdog watchdog(_params.watchdog(), 1000, true);
        watchdog.setWarningPeriod(1000);
        // Need to decide on aborting based on warning ticks instead of realtime
        // because this process can be suspended for an indeterminate amount of time.
        watchdog.setAbortTicks(1 + _params.watchdogAbortMillis() / 1000);

        // Main loop
        while (true) {

            doSleep();
            Timer::cacheElapsedSeconds();
            watchdog.reset(Timer::elapsedSecondsCached());

            // Terminate
            if (_hsm->doTerminate) {
                Terminator::setTerminating();
            }
            if (Terminator::isTerminating(true)) {
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
                    pipe.readData(c);

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
                    exportLiteralLimit = pipe.readData(c)[0];

                } else if (c == CLAUSE_PIPE_FILTER_IMPORT) {
                    incomingClauses = pipe.readData(c);
                    int epoch = popLast(incomingClauses);
                    InplaceClauseAggregation agg(incomingClauses);
                    int winningSolverId = agg.successfulSolver();
                    int bufferRevision = agg.maxRevision();
                    agg.stripToRawBuffer();
                    LOGGER(_log, V5_DEBG, "DO filter clauses\n");
                    engine.setClauseBufferRevision(bufferRevision);
                    auto filter = engine.filterSharing(incomingClauses);
                    LOGGER(_log, V5_DEBG, "filter result has size %i\n", filter.size());
                    pipe.writeData(std::move(filter), {epoch}, CLAUSE_PIPE_FILTER_IMPORT);
                    if (winningSolverId >= 0) {
                        LOGGER(_log, V4_VVER, "winning solver ID: %i\n", winningSolverId);
                        engine.setWinningSolverId(winningSolverId);
                    }

                } else if (c == CLAUSE_PIPE_DIGEST_IMPORT) {
                    auto filter = pipe.readData(c);
                    int epoch = popLast(filter);
                    doImportClauses(engine, incomingClauses, &filter, -1, epoch);
                    pipe.writeData({engine.getLastAdmittedClauseShare().nbAdmittedLits}, CLAUSE_PIPE_DIGEST_IMPORT);

                } else if (c == CLAUSE_PIPE_DIGEST_IMPORT_WITHOUT_FILTER) {
                    incomingClauses = pipe.readData(c);
                    bool stateless = popLast(incomingClauses)==1;
                    int epoch = popLast(incomingClauses);
                    InplaceClauseAggregation agg(incomingClauses);
                    int bufferRevision = agg.maxRevision();
                    agg.stripToRawBuffer();
                    doImportClauses(engine, incomingClauses, nullptr, bufferRevision, epoch, stateless);

                } else if (c == CLAUSE_PIPE_RETURN_CLAUSES) {
                    LOGGER(_log, V5_DEBG, "DO return clauses\n");
                    auto clauses = pipe.readData(c);
                    int bufferRevision = popLast(clauses);
                    engine.setClauseBufferRevision(bufferRevision);
                    engine.returnClauses(clauses);

                } else if (c == CLAUSE_PIPE_DIGEST_HISTORIC) {
                    LOGGER(_log, V5_DEBG, "DO digest historic clauses\n");
                    auto data = pipe.readData(c);
                    int bufferRevision = popLast(data);
                    int epochEnd = popLast(data);
                    int epochBegin = popLast(data);
                    engine.setClauseBufferRevision(bufferRevision);
                    engine.digestHistoricClauses(epochBegin, epochEnd, data);

                } else if (c == CLAUSE_PIPE_REDUCE_THREAD_COUNT) {
                    LOGGER(_log, V3_VERB, "DO reduce thread count\n");
                    pipe.readData(c);
                    engine.reduceActiveThreadCount();

                } else if (c == CLAUSE_PIPE_SET_THREAD_COUNT) {
                    LOGGER(_log, V3_VERB, "DO set thread count\n");
                    int nbThreads = pipe.readData(c)[0];
                    engine.setActiveThreadCount(nbThreads);

                } else if (c == CLAUSE_PIPE_START_NEXT_REVISION) {
                    LOGGER(_log, V5_DEBG, "DO start next revision\n");
                    auto data = pipe.readData(c);
                    _last_present_revision = popLast(data);
                    _desired_revision = popLast(data);

                } else if (c == CLAUSE_PIPE_UPDATE_BEST_FOUND_OBJECTIVE_COST) {
                    LOGGER(_log, V5_DEBG, "DO update best found objective cost\n");
                    auto data = pipe.readData(c);
                    long long bestCost = * (long long*) data.data();
                    engine.updateBestFoundObjectiveCost(bestCost);

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
                    long long bestFoundObjectiveCost = engine.getBestFoundObjectiveCost();
                    int costAsInts[sizeof(long long)/sizeof(int)];
                    memcpy(costAsInts, &bestFoundObjectiveCost, sizeof(long long));
                    std::vector<int> metadata;
                    for (int i = 0; i < sizeof(long long)/sizeof(int); i++)
                        metadata.push_back(costAsInts[i]);
                    metadata.push_back(numCollectedLits);
                    metadata.push_back(successfulSolverId);
                    pipe.writeData(std::move(clauses), metadata, CLAUSE_PIPE_PREPARE_CLAUSES);
                }
                collectClauses = false;
            }

            // Do not check solved state if the current 
            // revision has already been solved
            if (lastSolvedRevision == _last_imported_revision) continue;

            // Check solved state
            int resultCode = engine.solveLoop();
            if (resultCode == 99) {
                // Preprocessing result found
                std::vector<int> fPre = std::move(engine.getPreprocessedFormula());
                pipe.writeData(std::move(fPre), CLAUSE_PIPE_SUBMIT_PREPROCESSED_FORMULA);

            } else if (resultCode >= 0 && !_has_solution) {
                // Solution found!
                auto& result = engine.getResult();
                result.id = _config.jobid;
                if (_hsm->doTerminate || result.revision < _desired_revision) {
                    // Result obsolete
                    continue;
                }
                assert(result.revision == _last_imported_revision);

                std::vector<int> solutionVec = result.extractSolution();
                int solutionRevision = result.revision;
                int winningInstance = result.winningInstanceId;
                unsigned long globalStartOfSuccessEpoch = result.globalStartOfSuccessEpoch;

                LOGGER(_log, V5_DEBG, "DO write solution (winning instance: %i)\n", winningInstance);
                pipe.writeData(std::move(solutionVec), {
                    solutionRevision, winningInstance, ((int*) &globalStartOfSuccessEpoch)[0], ((int*) &globalStartOfSuccessEpoch)[1], result.result
                }, CLAUSE_PIPE_SOLUTION);
                lastSolvedRevision = solutionRevision;
                LOGGER(_log, V5_DEBG, "DONE write solution\n");
                _has_solution = true;
            }
        }

        Terminator::setTerminating();
        // This call ends the program.
        checkTerminate(engine, true, exitStatus);
        abort(); // should be unreachable

        // Shared memory will be cleaned up by the parent process.
    }

    bool checkTerminate(SatEngine& engine, bool force, int exitStatus = 0, std::function<void()> cbAtForcedExit = [](){}) {
        bool terminate = Terminator::isTerminating(true);
        if (terminate && force) {
            // clean up all resources which MUST be cleaned up (e.g., child processes)
            engine.cleanUp(true);
            cbAtForcedExit();
            _hsm->didTerminate = true;
            // terminate yourself
            assert(exitStatus != 9); // not hard-killed - wouldn't make sense
            LOGGER(_log, V4_VVER, "Exiting\n");
            _log.flush();
            Process::doExit(exitStatus);
        }
        return terminate;
    }

    void readFormulaAndAssumptionsFromSharedMem(SatEngine& engine, int revision) {

        float time = Timer::elapsedSeconds();

        size_t fSize, aSize;
        Checksum checksum;
        if (revision == 0) {
            fSize = _hsm->fSize;
            aSize = _hsm->aSize;
            checksum = _hsm->chksum;
        } else {
            size_t* fSizePtr = (size_t*) accessMemory(_shmem_id + ".fsize." + std::to_string(revision), sizeof(size_t));
            size_t* aSizePtr = (size_t*) accessMemory(_shmem_id + ".asize." + std::to_string(revision), sizeof(size_t));
            Checksum* chk = (Checksum*) accessMemory(_shmem_id + ".checksum." + std::to_string(revision), sizeof(Checksum));
            fSize = *fSizePtr;
            aSize = *aSizePtr;
            checksum = *chk;
        }

        std::string formulaShmemId = _shmem_id + ".formulae." + std::to_string(revision);
        int* descIdPtr = (int*) accessMemory(_shmem_id + ".descid." + std::to_string(revision),
            sizeof(int), SharedMemory::ARBITRARY, false);
        if (descIdPtr) {
            const int descId = *descIdPtr;
            formulaShmemId = "/edu.kit.iti.mallob.jobdesc." + std::to_string(descId);
        }

        const int* fPtr = (const int*) accessMemory(formulaShmemId,
            sizeof(int) * fSize, SharedMemory::READONLY);
        const int* aPtr = (const int*) accessMemory(_shmem_id + ".assumptions." + std::to_string(revision),
            sizeof(int) * aSize, SharedMemory::READONLY);

        LOGGER(_log, V5_DEBG, "FORMULA rev. %i : %s\n", revision, StringUtils::getSummary(fPtr, fSize).c_str());
        if (fSize > 0 && (fPtr[0] == 0)) {
            LOGGER(_log, V0_CRIT, "[ERROR] rev. %i begins with a zero\n", revision);  
            _log.flush();
            abort();
        }
        if (fSize > 0 && (fPtr[fSize-1] != 0)) {
            LOGGER(_log, V0_CRIT, "[ERROR] rev. %i does not end with a zero\n", revision);  
            _log.flush();
            abort();
        }
        LOGGER(_log, V5_DEBG, "ASSUMPTIONS rev. %i : %s\n", revision, StringUtils::getSummary(aPtr, aSize).c_str());

        if (_params.copyFormulaeFromSharedMem()) {
            // Copy formula and assumptions to your own local memory
            _read_formulae.emplace_back(fPtr, fPtr+fSize);
            _read_assumptions.emplace_back(aPtr, aPtr+aSize);

            // Reference the according positions in local memory when forwarding the data
            engine.appendRevision(revision, {fSize, _read_formulae.back().data(),
                aSize, _read_assumptions.back().data(), checksum}, revision == _desired_revision);
        } else {
            // Let the solvers read from shared memory directly
            engine.appendRevision(revision, {fSize, fPtr, aSize, aPtr, checksum}, revision == _desired_revision);
        }

        time = Timer::elapsedSeconds() - time;
        LOGGER(_log, V3_VERB, "Read formula rev. %i (size:%lu,%lu, chk:%lu,%x) from shared memory in %.4fs\n", revision, fSize, aSize,
            checksum.count(), checksum.get(), time);
    }

    void* accessMemory(const std::string& shmemId, size_t size, SharedMemory::AccessMode accessMode = SharedMemory::ARBITRARY, bool abortOnFailure = true) {
        void* ptr = SharedMemory::access(shmemId, size, accessMode);
        if (abortOnFailure && ptr == nullptr) {
            LOGGER(_log, V0_CRIT, "[ERROR] Could not access shmem %s\n", shmemId.c_str());  
            abort();
        }
        return ptr;
    }

    void importRevisions(SatEngine& engine) {
        while (_last_imported_revision < _last_present_revision) {
            if (checkTerminate(engine, false)) return;
            _last_imported_revision++;
            readFormulaAndAssumptionsFromSharedMem(engine, _last_imported_revision);
            _has_solution = false;
        }
    }

    void doSleep() {
        // Wait until something happens
        // (can be interrupted by Fork::wakeUp(hsm->childPid))
        usleep(1000 /*1 millisecond*/);
    }
};


#pragma once

#include <sys/types.h>

#include "../solvers/portfolio_solver_interface.hpp"
#include "app/sat/execution/engine.hpp"
#include "data/checksum.hpp"
#include "sat_process_config.hpp"

struct SatSharedMemory {

    SatProcessConfig config;

    // Meta data parent->child
    int fSize;
    int aSize;
    int desiredRevision;

    // Instructions parent->child
    bool doBegin {false};
    bool doExport {false};
    bool doFilterImport {false};
    bool doDigestImportWithFilter {false};
    bool doDigestImportWithoutFilter {false};
    bool doReturnClauses {false};
    bool doDigestHistoricClauses {false};
    bool doDumpStats {false};
    bool doStartNextRevision {false};
    bool doTerminate {false};
    bool doCrash {false};
    bool doReduceThreadCount {false};

    // Responses child->parent
    bool didExport {false};
    bool didFilterImport {false};
    bool didDigestImport {false};
    bool didReturnClauses {false};
    bool didDigestHistoricClauses {false};
    bool didDumpStats {false};
    bool didStartNextRevision {false};
    bool didTerminate {false};
    bool didReduceThreadCount {false};

    // State alerts child->parent
    bool isInitialized {false};
    bool hasSolution {false};
    SatResult result {UNKNOWN};
    int solutionRevision {-1};
    int winningInstance {-1};
    unsigned long globalStartOfSuccessEpoch;
    
    // Clause buffers: parent->child
    int exportLiteralLimit;
    int importBufferRevision {-1};
    Checksum importChecksum;
    int importEpoch;
    int historicEpochBegin;
    int historicEpochEnd;
    int winningSolverId {-1};
    int numCollectedLits {0};
    
    // Clause buffers: child->parent
    Checksum exportChecksum;
    SatEngine::LastAdmittedStats lastAdmittedStats;
    int successfulSolverId {-1};
};

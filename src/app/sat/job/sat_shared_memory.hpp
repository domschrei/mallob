
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

    // Signals parent->child
    bool doBegin {false};
    bool doTerminate {false};
    bool doCrash {false};
    bool pipeChildReadyToWrite {false};
    bool pipeTerminate {false};

    // Signals child->parent
    bool didBegin {false};
    bool didTerminate {false};
    
    // State alerts child->parent
    bool isInitialized {false};
    
    // Clause buffers: parent->child
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

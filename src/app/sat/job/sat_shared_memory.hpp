
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
    Checksum chksum;

    // Signals parent->child
    volatile bool doTerminate {false};
    volatile bool doCrash {false};
    //bool pipeChildReadyToWrite {false};
    //bool pipeDoTerminate {false};
    //bool pipeDidTerminate {false};

    // Signals child->parent
    volatile bool didStart {false};
    volatile bool didTerminate {false};
    
    // State alerts child->parent
    volatile bool isInitialized {false};
    
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

    // Pipe data buffer size for each direction
    size_t pipeBufSize {262144};
};

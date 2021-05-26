
#ifndef DOMPASCH_MALLOB_HORDE_SHARED_MEMORY_HPP
#define DOMPASCH_MALLOB_HORDE_SHARED_MEMORY_HPP

#include <sys/types.h>

#include "hordesat/solvers/portfolio_solver_interface.hpp"
#include "data/checksum.hpp"

struct HordeSharedMemory {

    // Meta data parent->child
    int fSize;
    int aSize;
    int revision;

    // Instructions parent->child
    bool doExport;
    bool doImport;
    bool doDumpStats;
    bool doStartNextRevision;
    bool doTerminate;

    // Responses child->parent
    bool didExport;
    bool didImport;
    bool didDumpStats;
    bool didStartNextRevision;
    bool didTerminate;

    // State alerts child->parent
    bool isSpawned;
    bool isInitialized;
    bool hasSolution;
    SatResult result;
    int solutionRevision;
    
    // Clause buffers: parent->child
    int exportBufferMaxSize;
    int importBufferSize;
    Checksum importChecksum;
    
    // Clause buffers: child->parent
    int exportBufferTrueSize;
    Checksum exportChecksum;
};

#endif
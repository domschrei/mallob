
#ifndef DOMPASCH_MALLOB_HORDE_SHARED_MEMORY_HPP
#define DOMPASCH_MALLOB_HORDE_SHARED_MEMORY_HPP

#include <sys/types.h>

#include "hordesat/solvers/portfolio_solver_interface.hpp"

struct HordeSharedMemory {

    // Meta data parent->child
    int portfolioRank;
    int portfolioSize;

    // Instructions parent->child
    bool doExport;
    bool doImport;
    bool doDumpStats;
    bool doUpdateRole;
    bool doInterrupt;
    bool doTerminate;

    // Responses child->parent
    bool didExport;
    bool didImport;
    bool didDumpStats;
    bool didUpdateRole;
    bool didInterrupt;
    bool didTerminate;

    // State alerts child->parent
    bool isSpawned;
    bool isInitialized;
    bool hasSolution;
    SatResult result;
	int solutionSize;
    
    // Clause buffers: parent->child
    int exportBufferMaxSize;
    int importBufferSize;
    
    // Clause buffers: child->parent
    int exportBufferTrueSize;
};

#endif
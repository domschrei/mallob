
#ifndef DOMPASCH_MALLOB_HORDE_SHARED_MEMORY_HPP
#define DOMPASCH_MALLOB_HORDE_SHARED_MEMORY_HPP

#include <sys/types.h>

#include "hordesat/solvers/portfolio_solver_interface.hpp"
#include "data/checksum.hpp"
#include "horde_config.hpp"

struct HordeSharedMemory {

    HordeConfig config;

    // Meta data parent->child
    int fSize;
    int aSize;
    int desiredRevision;

    // Instructions parent->child
    bool doBegin;
    bool doExport;
    bool doFilterImport;
    bool doDigestImportWithFilter;
    bool doDigestImportWithoutFilter;
    bool doReturnClauses;
    bool doDumpStats;
    bool doStartNextRevision;
    bool doTerminate;

    // Responses child->parent
    bool didExport;
    bool didFilterImport;
    bool didDigestImport;
    bool didReturnClauses;
    bool didDumpStats;
    bool didStartNextRevision;
    bool didTerminate;

    // State alerts child->parent
    bool isInitialized;
    bool hasSolution;
    SatResult result;
    int solutionRevision;
    
    // Clause buffers: parent->child
    int exportBufferAllocatedSize;
    int exportBufferMaxSize;
    int importBufferMaxSize;
    int importBufferSize;
    int importBufferRevision;
    int returnedBufferSize;
    Checksum importChecksum;
    
    // Clause buffers: child->parent
    int exportBufferTrueSize;
    Checksum exportChecksum;
    int filterSize;
    int lastNumClausesToImport;
    int lastNumAdmittedClausesToImport;
};

#endif
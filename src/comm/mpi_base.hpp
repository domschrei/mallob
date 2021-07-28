
#ifndef DOMPASCH_MALLOB_MPI_BASE_HPP
#define DOMPASCH_MALLOB_MPI_BASE_HPP

// Turn off incompatible function types warning in openmpi
#define OMPI_SKIP_MPICXX 1
#include <mpi.h>

#define MPICALL(cmd, str) {int err = cmd; chkerr(err);}
void chkerr(int err);

#endif

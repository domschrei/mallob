#!/bin/bash

nompi() {
    echo "Cannot find valid MPI installation! Scanning for potential MPI dirs ..."
    find /usr /lib /intel64 -name "mpi.h"|xargs dirname
    echo "Scan finished. Add installation path manually to $0."
    exit 1
}

mpiroot=""
mpiinclude=""

if [ -d /usr/include/mpi ]; then
    mpiroot="/usr/include/mpi"
elif [ -d /usr/lib/openmpi ]; then
    mpiroot="/usr/lib/openmpi"
elif [ -d /usr/lib/x86_64-linux-gnu/openmpi/ ]; then
    mpiroot="/usr/lib/x86_64-linux-gnu/openmpi/"
else
    nompi
fi
echo mpiroot: $mpiroot

if [ -f ${mpiroot}include/mpi.h ]; then
    mpiinclude=${mpiroot}include/
elif [ -f $mpiroot/mpi.h ]; then
    mpiinclude=$mpiroot
else
    nompi
fi
echo mpiinclude: $mpiinclude

(cd src/hordesat && bash fetch_and_build_solvers.sh)

cmd="MPI_ROOT=$mpiroot MPI_INCLUDE=$mpiinclude make"
echo $cmd
bash -c "$cmd"
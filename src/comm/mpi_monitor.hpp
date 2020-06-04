
#ifndef DOMPASCH_MALLOB_MPI_MONITOR_H
#define DOMPASCH_MALLOB_MPI_MONITOR_H

#include "mympi.hpp"

void initcall(const char* op);
void endcall();

class Worker;
void mpiMonitor(Worker* worker);

#endif
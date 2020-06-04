
#ifndef MALLOB_MEMORY_USAGE_H
#define MALLOB_MEMORY_USAGE_H

#include <sys/time.h>
#include <sys/resource.h>

#include <unistd.h>
#include <ios>
#include <iostream>
#include <fstream>
#include <string>

namespace Proc {

    pid_t getPid();
    long getTid();

    // https://stackoverflow.com/a/671389
    void getSelfMemAndSchedCpu(int& cpu, double& vm_usage, double& resident_set);

    /*
    If successful, returns the used CPU ratio and the share of time it spent in kernel mode.
    Measured SINCE the previous call to this method. The first call initializes
    the measurement and is guaranteed to fail.
    */
    bool getThreadCpuRatio(long tid, double& cpuRatio, float& sysShare);

}

#endif
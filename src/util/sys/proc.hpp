
#ifndef MALLOB_MEMORY_USAGE_H
#define MALLOB_MEMORY_USAGE_H

#include <sys/time.h>
#include <sys/resource.h>

#include <unistd.h>
#include <ios>
#include <iostream>
#include <fstream>
#include <string>
#include <map>

#include "util/sys/threading.hpp"

class Proc {

private:
    static std::map<long, float> _tid_lastcall;
    static std::map<long, unsigned long> _tid_utime;
    static std::map<long, unsigned long> _tid_stime;

    static Mutex _tid_lock;

public:
    static pid_t getPid();
    static long getTid();

    // https://stackoverflow.com/a/671389
    static void getSelfMemAndSchedCpu(int& cpu, double& vm_usage, double& resident_set);

    /*
    If successful, returns the used CPU ratio and the share of time it spent in kernel mode.
    Measured SINCE the previous call to this method. The first call initializes
    the measurement and is guaranteed to fail.
    */
    static bool getThreadCpuRatio(long tid, double& cpuRatio, float& sysShare);

};

#endif
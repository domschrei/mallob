
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

/*
Interface to some process-related information from the /proc filesystem.
*/
class Proc {

private:
    struct CpuInfo { 
        double uticks = 0, sticks = 0, passedSecs = 0;
        CpuInfo() = default;
        CpuInfo(CpuInfo& other) = default; 
        CpuInfo& operator=(CpuInfo&& other) = default;
        CpuInfo& operator=(const CpuInfo& other) = default;
    };
    static std::map<long, CpuInfo> _cpu_info_per_tid;
    static Mutex _cpu_info_lock;

public:

    static pid_t getPid();
    static pid_t getParentPid();
    static long getTid();

    struct RuntimeInfo {int cpu = -1; double vmUsage = 0; double residentSetSize = 0;};
    enum SubprocessMode {RECURSE, FLAT};
    static RuntimeInfo getRuntimeInfo(pid_t pid, SubprocessMode mode);

    /*
    If successful, returns the used CPU ratio and the share of time it spent in kernel mode.
    Measured SINCE the previous call to this method. The first call initializes
    the measurement and is guaranteed to fail.
    */
    static bool getThreadCpuRatio(long tid, double& cpuRatio, float& sysShare);

    static float getUptime();

};

#endif

#ifndef MALLOB_MEMORY_USAGE_H
#define MALLOB_MEMORY_USAGE_H

#include <unistd.h>
#include <map>

#include "util/sys/threading.hpp"

/*
Interface to some process-related information from the /proc filesystem.
*/
class Proc {

private:
    struct CpuInfo {
        unsigned long long userTicksSinceStart = 0;
        unsigned long long systemTicksSinceStart = 0;
        double elapsedSecsSinceStart = 0;
    };
    static std::map<long, CpuInfo> _cpu_info_per_tid;
    static Mutex _cpu_info_lock;

public:

    static pid_t getPid();
    static pid_t getParentPid();
    static long getTid();

    static void nameThisThread(const char* nameMax16Chars);

    struct RuntimeInfo {int cpu = -1; double vmUsage = 0; double residentSetSize = 0;};
    enum SubprocessMode {RECURSE, FLAT};
    static RuntimeInfo getRuntimeInfo(pid_t pid, SubprocessMode mode);

    static std::vector<pid_t> getChildren(pid_t pid);

    static std::pair<unsigned long, unsigned long> getMachineFreeAndTotalRamKbs();
    static long getRecursiveProportionalSetSizeKbs(pid_t pid);

    /*
    If successful, returns the used CPU ratio and the share of time it spent in kernel mode.
    Measured SINCE the previous call to this method. The first call initializes
    the measurement and is guaranteed to fail.
    */
    static bool getThreadCpuRatio(long tid, double& cpuRatio, float& sysShare);

    static float getUptime();

};

#endif
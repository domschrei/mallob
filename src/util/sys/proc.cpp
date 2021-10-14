

#include <sys/time.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <map>
#include "util/assert.hpp"
#include <ios>
#include <iostream>
#include <fstream>
#include <string>
#include <set>

#include "util/sys/fileutils.hpp"
#include "proc.hpp"
#include "util/sys/timer.hpp"
#include "util/logger.hpp"

std::map<long, Proc::CpuInfo> Proc::_cpu_info_per_tid;
Mutex Proc::_cpu_info_lock;

pid_t Proc::getPid() {
    return getpid();
}

pid_t Proc::getParentPid() {
    return getppid();
}

long Proc::getTid() {
    return syscall(SYS_gettid);
}

// https://stackoverflow.com/a/671389
Proc::RuntimeInfo Proc::getRuntimeInfo(pid_t pid, SubprocessMode mode) {

    using std::ios_base;
    using std::ifstream;
    using std::string;

    RuntimeInfo info;

    auto statFile = "/proc/" + std::to_string(pid) + "/stat";
    ifstream stat_stream(statFile.c_str(), ios_base::in);
    if (!stat_stream.good()) return info;

    // dummy vars for leading entries in stat that we don't care about
    string str_pid, comm, state, ppid, pgrp, session, tty_nr;
    string tpgid, flags, minflt, cminflt, majflt, cmajflt;
    string utime, stime, cutime, cstime, priority, nice;
    string O, itrealvalue, starttime;

    // the two fields we want
    unsigned long vsize = 0;
    long rss = 0;

    stat_stream >> str_pid >> comm >> state >> ppid >> pgrp >> session >> tty_nr
                >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt
                >> utime >> stime >> cutime >> cstime >> priority >> nice
                >> O >> itrealvalue >> starttime >> vsize >> rss; // don't care about the rest

    stat_stream.close();

    long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024; // in case x86-64 is configured to use 2MB pages
    info.vmUsage = vsize / 1024.0;
    info.residentSetSize = rss * page_size_kb;
    info.cpu = sched_getcpu();

    if (mode != RECURSE) return info;

    // Read all child PIDs
    std::vector<pid_t> childPids = getChildren(pid);

    // Get runtime info for each PID
    int numChildren = 0;
    for (int childPid : childPids) {
        RuntimeInfo childInfo = getRuntimeInfo(childPid, mode);
        info.vmUsage += childInfo.vmUsage;
        info.residentSetSize += childInfo.residentSetSize;
        numChildren++;
    }
    log(V5_DEBG, "%i : %i children\n", pid, numChildren);

    return info;
}

float Proc::getUptime() {
    // Get uptime in seconds
    std::ifstream uptime_stream("/proc/uptime", std::ios_base::in);
    float uptime;
    uptime_stream >> uptime;
    uptime_stream.close();
    return uptime;
}

bool Proc::getThreadCpuRatio(long tid, double& cpuRatio, float& sysShare) {
   
    using std::ios_base;
    using std::ifstream;
    using std::string;

    static unsigned long hertz = sysconf(_SC_CLK_TCK);

    double uptime = getUptime();

    // Get stats of interest
    std::string filepath = "/proc/" + std::to_string(getPid()) + "/task/" + std::to_string(tid) + "/stat";
    ifstream stat_stream(filepath, ios_base::in);
    if (!stat_stream.good()) return false;

    // dummy vars for leading entries
    string pid, comm, state, ppid, pgrp, session, tty_nr;
    string tpgid, flags, minflt, cminflt, majflt, cmajflt;
    string cutime, cstime, priority, nice;
    string O, itrealvalue;

    double uticks = 0, sticks = 0, starttime = 0;
    stat_stream >> pid >> comm >> state >> ppid >> pgrp >> session >> tty_nr
                >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt
                >> uticks >> sticks >> cutime >> cstime >> priority >> nice
                >> O >> itrealvalue >> starttime;
    stat_stream.close();

    auto lock = _cpu_info_lock.getLock();

    CpuInfo& info = _cpu_info_per_tid[tid];

    auto ticks = uticks + sticks;
    auto ticksDiff = ticks - (info.uticks + info.sticks);
    auto sTicksDiff = sticks - info.sticks;

    auto passedSecs = uptime - (starttime / hertz);
    auto passedSecsDiff = passedSecs - info.passedSecs;
    //log(V4_VVER, "PROC tid=%lu spent %.2f ticks over %.2f secs\n", tid, ticksDiff, passedSecsDiff);

    cpuRatio = passedSecsDiff == 0 ? 0 : ((ticksDiff / hertz) / passedSecsDiff);
    sysShare = ticksDiff == 0 ? 0 : sTicksDiff / ticksDiff;

    info.uticks = uticks;
    info.sticks = sticks;
    info.passedSecs = passedSecs;

    return true;
}

std::vector<pid_t> Proc::getChildren(pid_t pid) {
    // Read all child PIDs
    std::set<pid_t> childPids;
    for (const auto& childFile : FileUtils::glob("/proc/" + std::to_string(pid) + "/task/*/children")) {

        std::ifstream children_stream(childFile.c_str(), std::ios_base::in);
        if (!children_stream.good()) continue;
        
        pid_t childPid;
        while (children_stream >> childPid) {
            childPids.insert(childPid);
        }
    }
    return std::vector<pid_t>(childPids.begin(), childPids.end());
}

long Proc::getRecursiveProportionalSetSizeKbs(pid_t pid) {

    long memory = 0;
    std::ifstream smapsRollup("/proc/" + std::to_string(pid) + "/smaps_rollup", std::ios_base::in);
    if (!smapsRollup.good()) return memory;
    std::string line;
    while (getline(smapsRollup, line)) {
        if (line.size() > 4 && line.substr(0, 4) == "Pss:") {
            // Anatomy of a line (example):
            // "Pss:              513166 kB"
            std::string stringOfInts = "";
            for (char c : line) stringOfInts += std::to_string((int)(c)) + ",";
            // Find start and end indices of memory value
            int i = 4;
            while (line[i] == ' ') i++;
            int j = i+1;
            while (line[j] != ' ') j++;
            std::string memValStr = line.substr(i, j-i);
            memory += atol(memValStr.c_str());
        }
    }

    auto childPids = getChildren(pid);
    for (pid_t child : childPids) memory += getRecursiveProportionalSetSizeKbs(child);

    return memory;
}

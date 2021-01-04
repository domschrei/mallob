
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <map>
#include <assert.h>

#include "proc.hpp"
#include "util/sys/timer.hpp"
#include "util/console.hpp"

robin_hood::unordered_map<long, Proc::CpuInfo> Proc::_cpu_info_per_tid;
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

    // Recursively read memory usage of all children
    if (mode == RECURSE) {
        auto childFile = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) + "/children";
        ifstream children_stream(childFile.c_str(), ios_base::in);
        if (!children_stream.good()) return info;

        pid_t childPid;
        while (children_stream >> childPid) {
            RuntimeInfo childInfo = getRuntimeInfo(childPid, mode);
            info.vmUsage += childInfo.vmUsage;
            info.residentSetSize += childInfo.residentSetSize;
        }
    }

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
    Console::log(Console::VVERB, "PROC tid=%lu spent %.2f ticks over %.2f secs", tid, ticksDiff, passedSecsDiff);

    cpuRatio = passedSecsDiff == 0 ? 0 : ((ticksDiff / hertz) / passedSecsDiff);
    sysShare = ticksDiff == 0 ? 0 : sTicksDiff / ticksDiff;

    info.uticks = uticks;
    info.sticks = sticks;
    info.passedSecs = passedSecs;

    return true;
}

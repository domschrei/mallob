
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <map>

#include "proc.hpp"
#include "util/sys/timer.hpp"


// Static fields
std::map<long, float>         Proc::_tid_lastcall;
std::map<long, unsigned long> Proc::_tid_utime;
std::map<long, unsigned long> Proc::_tid_stime;
Mutex Proc::_tid_lock;


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

bool Proc::getThreadCpuRatio(long tid, double& cpuRatio, float& sysShare) {
   
    using std::ios_base;
    using std::ifstream;
    using std::string;

    /*
    // Get uptime in seconds
    ifstream uptime_stream("/proc/uptime", ios_base::in);
    unsigned long uptime;
    uptime_stream >> uptime;
    uptime_stream.close();
    */

    // Get hertz
    unsigned long hertz = sysconf(_SC_CLK_TCK);

    // Get current relative time
    float age = Timer::elapsedSeconds();

    // Get actual stats of interest
    std::string filepath = "/proc/" + std::to_string(getPid()) + "/task/" + std::to_string(tid) + "/stat";
    ifstream stat_stream(filepath, ios_base::in);
    if (!stat_stream.good()) return false;
    // dummy vars for leading entries in stat that we don't care about
    string pid, comm, state, ppid, pgrp, session, tty_nr;
    string tpgid, flags, minflt, cminflt, majflt, cmajflt;
    string cutime, cstime, priority, nice;
    string O, itrealvalue, starttime;
    // the two fields we want
    unsigned long utime = 0;
    unsigned long stime = 0;
    stat_stream >> pid >> comm >> state >> ppid >> pgrp >> session >> tty_nr
                >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt
                >> utime >> stime >> cutime >> cstime >> priority >> nice
                >> O >> itrealvalue >> starttime;
    stat_stream.close();

    auto lock = _tid_lock.getLock();

    if (!_tid_lastcall.count(tid)) {
        _tid_lastcall[tid] = Timer::elapsedSeconds();
        _tid_utime[tid] = utime;
        _tid_stime[tid] = stime;
        return false;
    }

    unsigned long utimeDiff = utime - _tid_utime[tid];
    unsigned long stimeDiff = stime - _tid_stime[tid];
    unsigned long totalDiff = utimeDiff + stimeDiff;
    float elapsedTime = age - _tid_lastcall[tid];

    // Compute result
    cpuRatio = 100 * (float(totalDiff) / hertz) / elapsedTime;
    sysShare = totalDiff == 0 ? 0 : float(stimeDiff) / totalDiff;

    _tid_lastcall[tid] = age;
    _tid_utime[tid] = utime;
    _tid_stime[tid] = stime;

    return true;
}

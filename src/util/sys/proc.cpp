

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
#include <pthread.h>

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

void Proc::nameThisThread(const char* nameMax16Chars) {
    pthread_setname_np(pthread_self(), nameMax16Chars);
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
    LOG(V5_DEBG, "%i : %i children\n", pid, numChildren);

    return info;
}

std::pair<unsigned long, unsigned long> Proc::getMachineFreeAndTotalRamKbs() {
    std::pair<unsigned long, unsigned long> freeAndTotalRamKbs;

    using std::ios_base;
    using std::ifstream;
    using std::string;

    std::string statFile = "/proc/meminfo";
    ifstream stat_stream(statFile.c_str(), ios_base::in);
    if (!stat_stream.good()) return freeAndTotalRamKbs;

    const std::string labelTotal = "MemTotal:";
    const std::string labelAvailable = "MemAvailable:";
    int numFound = 0;
    const int numNeeded = 2;
    while (numFound < numNeeded) {
        std::string label; 
        unsigned long value;
        std::string unit;
        stat_stream >> label >> value >> unit;
        if (label == labelAvailable) {
            assert(unit == "kB");
            freeAndTotalRamKbs.first = value;
            numFound++;
            //LOG(V2_INFO, "Available RAM on this machine: %lu kB\n", value);
        } else if (label == labelTotal) {
            assert(unit == "kB");
            freeAndTotalRamKbs.second = value;
            numFound++;
            //LOG(V2_INFO, "Total RAM on this machine: %lu kB\n", value);
        }
    }

    stat_stream.close();
    return freeAndTotalRamKbs;
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
    /*
    See `man proc`, section `/proc/[PID]/stat`:
    Field "comm" is in parentheses and may contain whitespaces, 
    so it can break word-by-word reading
               1     2    3
    pid (comm) state ppid pgrp
    4       5      6     7     8
    session tty_nr tpgid flags minflt
    9       10     11      12!   13!
    cminflt majflt cmajflt utime ctime
    14     15     16       17   18
    cutime cstime priority nice O 
    19          20!
    itrealvalue starttime
    */
    static const int postCommIdxUserTicks = 12;
    static const int postCommIdxSystemTicks = 13;

    // Get stream for the stats of interest
    std::string filepath = "/proc/" + std::to_string(getPid()) + "/task/" + std::to_string(tid) + "/stat";
    ifstream stat_stream(filepath, ios_base::in);
    if (!stat_stream.good()) return false;

    // Read stats carefully
    unsigned long long userTicksSinceStart = 0, systemTicksSinceStart = 0, starttime = 0;
    bool endedComm = false;
    int readWordsAfterComm = 0;
    bool readEverything = false;
    string word;
    //string wholeLine;
    while (stat_stream >> word) {
        //wholeLine += word + "|";
        if (word[word.size()-1] == ')') {
            endedComm = true;
        } else if (endedComm) {
            readWordsAfterComm++;
            if (readWordsAfterComm == postCommIdxUserTicks) {
                userTicksSinceStart = std::stoull(word);
            } else if (readWordsAfterComm == postCommIdxSystemTicks) {
                systemTicksSinceStart = std::stoull(word);
                readEverything = true;
                break;
            }
        }
    }
    stat_stream.close();
    //wholeLine = wholeLine.substr(0, wholeLine.size()-1);
    //log(V5_DEBG, "cpu %s\n", wholeLine.c_str());
    if (!readEverything) return false;

    // Get elapsed seconds of this program (only used relatively)
    double elapsedSecsSinceStart = Timer::elapsedSeconds();
    
    // Fetch previous CPU info for this thread 
    auto lock = _cpu_info_lock.getLock();
    bool prevInfoPresent = _cpu_info_per_tid.count(tid);
    if (prevInfoPresent) {
        // Read previous CPU info
        CpuInfo& info = _cpu_info_per_tid[tid];

        // Compute delta since previous query
        double deltaSecs = elapsedSecsSinceStart - info.elapsedSecsSinceStart;
        unsigned long long deltaUserTicks = userTicksSinceStart - info.userTicksSinceStart;
        unsigned long long deltaSystemTicks = systemTicksSinceStart - info.systemTicksSinceStart;
        unsigned long long deltaOverallTicks = deltaUserTicks+deltaSystemTicks;

        // Infer CPU ratio and system time share
        double deltaSecsTimesHertz = deltaSecs*hertz;
        cpuRatio = deltaSecsTimesHertz == 0 ? 0 : 
            (deltaOverallTicks / deltaSecsTimesHertz);
        sysShare = deltaOverallTicks == 0 ? 0 : 
            ((double)deltaSystemTicks / deltaOverallTicks);
    }

    // Write current values to the thread's CPU info struct
    CpuInfo& info = _cpu_info_per_tid[tid];
    info.elapsedSecsSinceStart = elapsedSecsSinceStart;
    info.userTicksSinceStart = userTicksSinceStart;
    info.systemTicksSinceStart = systemTicksSinceStart;

    // Successful iff previous CPU info was available
    return prevInfoPresent;
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

    // 1st attempt: read "rollup" file integrating all memory pages (more efficient)
    // (present since Linux 4.14) 
    bool usingRollup = true;
    std::ifstream smaps("/proc/" + std::to_string(pid) + "/smaps_rollup", std::ios_base::in);
    if (!smaps.good()) {
        // 2nd attempt: read normal smaps file and sum up all "PSS" entries
        usingRollup = false;
        smaps = std::ifstream("/proc/" + std::to_string(pid) + "/smaps", std::ios_base::in);
        if (!smaps.good()) return memory;
    }

    // Iterate over lines of the file
    std::string line;
    while (getline(smaps, line)) {
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
            if (usingRollup) break; // only one relevant line to read
        }
    }

    // Recurse over all children
    auto childPids = getChildren(pid);
    for (pid_t child : childPids) memory += getRecursiveProportionalSetSizeKbs(child);

    return memory;
}

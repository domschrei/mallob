
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <map>

#include "proc.hpp"
#include "util/sys/timer.hpp"

namespace Proc {

   std::map<long, float> _tid_lastcall;
   std::map<long, unsigned long> _tid_utime;
   std::map<long, unsigned long> _tid_stime;

   pid_t getPid() {
      return getpid();
   }

   long getTid() {
      return syscall(SYS_gettid);
   }

   // https://stackoverflow.com/a/671389
   void getSelfMemAndSchedCpu(int& cpu, double& vm_usage, double& resident_set) {

      using std::ios_base;
      using std::ifstream;
      using std::string;

      vm_usage     = 0.0;
      resident_set = 0.0;

      // 'file' stat seems to give the most reliable results
      //
      ifstream stat_stream("/proc/self/stat", ios_base::in);

      // dummy vars for leading entries in stat that we don't care about
      //
      string pid, comm, state, ppid, pgrp, session, tty_nr;
      string tpgid, flags, minflt, cminflt, majflt, cmajflt;
      string utime, stime, cutime, cstime, priority, nice;
      string O, itrealvalue, starttime;

      // the two fields we want
      //
      unsigned long vsize = 0;
      long rss = 0;

      stat_stream >> pid >> comm >> state >> ppid >> pgrp >> session >> tty_nr
                  >> tpgid >> flags >> minflt >> cminflt >> majflt >> cmajflt
                  >> utime >> stime >> cutime >> cstime >> priority >> nice
                  >> O >> itrealvalue >> starttime >> vsize >> rss; // don't care about the rest

      stat_stream.close();

      long page_size_kb = sysconf(_SC_PAGE_SIZE) / 1024; // in case x86-64 is configured to use 2MB pages
      vm_usage     = vsize / 1024.0;
      resident_set = rss * page_size_kb;

      cpu = sched_getcpu();
   }

   bool getThreadCpuRatio(long tid, double& cpuRatio, float& sysShare) {
      
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

}
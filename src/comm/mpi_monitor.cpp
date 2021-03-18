
#include "mpi_monitor.hpp"

#include "util/sys/threading.hpp"
#include "util/sys/timer.hpp"
#include "util/sys/terminator.hpp"

Mutex callLock;
double doingMpiTasksTime;
std::string currentOp;

void initcall(const char* op) {
    callLock.lock();
    currentOp = op;
    doingMpiTasksTime = Timer::elapsedSeconds();
    callLock.unlock();
}
void endcall() {
    callLock.lock();
    doingMpiTasksTime = 0;
    currentOp = "";
    callLock.unlock();
}
std::string MyMpi::currentCall(double* callStart) {
    callLock.lock();
    *callStart = doingMpiTasksTime;
    std::string op = currentOp;
    callLock.unlock();
    return op;
}

void mpiMonitor() {
    while (!Terminator::isTerminating()) {
        double callStart = 0;
        std::string opName = MyMpi::currentCall(&callStart);
        std::string report = "MMPI %i active handles";
        if (callStart < 0.00001 || opName == "") {
            report += "\n";
            log(V3_VERB, report.c_str(), MyMpi::getNumActiveHandles());
        } else {
            double elapsed = Timer::elapsedSeconds() - callStart;
            report += ", in \"%s\" for %.3fs\n";
            log(V3_VERB, report.c_str(), MyMpi::getNumActiveHandles(), opName.c_str(), elapsed);
            if (elapsed > 60.0) {
                // Inside some MPI call for a minute
                log_return_false("MPI call takes too long - aborting\n");
                Process::doExit(1);
            }
        }
        usleep(1000 * 1000); // 1s
    }
}
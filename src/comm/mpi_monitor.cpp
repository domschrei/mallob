
#include "mpi_monitor.hpp"

#include "util/sys/threading.hpp"
#include "util/sys/timer.hpp"
#include "worker.hpp"

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

void mpiMonitor(Worker* worker) {
    while (!worker->_exiting) {
        double callStart = 0;
        std::string opName = MyMpi::currentCall(&callStart);
        std::string report = "MMPI %i active handles";
        if (callStart < 0.00001 || opName == "") {
            Console::log(Console::VERB, report.c_str(), MyMpi::getNumActiveHandles());
        } else {
            double elapsed = Timer::elapsedSeconds() - callStart;
            report += ", in \"%s\" for %.3fs";
            Console::log(Console::VERB, report.c_str(), MyMpi::getNumActiveHandles(), opName.c_str(), elapsed);
            if (elapsed > 60.0) {
                // Inside some MPI call for a minute
                Console::fail("MPI call takes too long - aborting");
                exit(1);
            }
        }
        usleep(1000 * 1000); // 1s
    }
}
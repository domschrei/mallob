
#include "utilities/Threading.h"
#include "timer.h"

#include "mympi.h"

bool turnedOff = false;
int handleId;
Mutex callLock;
double doingMpiTasksTime;
std::string currentOp;

void MyMpi::turnOffMonitor() {
    turnedOff = true;
}

void initcall(const char* op) {
    if (turnedOff) return;
    callLock.lock();
    currentOp = op;
    doingMpiTasksTime = Timer::elapsedSeconds();
    callLock.unlock();
}
void endcall() {
    if (turnedOff) return;
    callLock.lock();
    doingMpiTasksTime = 0;
    currentOp = "";
    callLock.unlock();
}
std::string MyMpi::currentCall(double* callStart) {
    if (turnedOff) return;
    callLock.lock();
    *callStart = doingMpiTasksTime;
    std::string op = currentOp;
    callLock.unlock();
    return op;
}

#include "utilities/Threading.h"
#include "timer.h"

#include "mympi.h"

int handleId;
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
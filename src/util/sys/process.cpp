
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <cstdlib>
#include <sys/wait.h>
#include <iostream>
#include <exception>
#include <execinfo.h>
#include <signal.h>
#include "util/assert.hpp"

#include "process.hpp"
#include "proc.hpp"
#include "util/logger.hpp"
#include "util/sys/stacktrace.hpp"
#include "util/sys/background_worker.hpp"

int Process::_rank;

Mutex Process::_children_mutex;
std::set<pid_t> Process::_children;
std::atomic_bool Process::_main_process;

std::atomic_bool Process::_exit_signal_caught = false;
std::atomic_int Process::_exit_signal = 0;

BackgroundWorker Process::_terminate_checker;


void doNothing(int signum) {
    // Do nothing, just return
    //std::cout << "WOKE_UP" << std::endl;
}

void handleSignal(int signum) {
    // Do not recursively catch signals (if something goes wrong in here)
    if (Process::_exit_signal_caught) return;

    Process::_exit_signal = signum;
    Process::_exit_signal_caught = true;
    
    if (signum == SIGSEGV || signum == SIGABRT) {
        // Try to write a trace of this thread found by gdb
        Process::writeTrace(Proc::getTid());
    }
    
    // If this is the main process, its main loop will detect termination.
    // If this is a subprocess, exit immediately
    if (!Process::_main_process) Process::doExit(signum);
}

void Process::doExit(int retval) {
    // Set exit signal to make terminate checker stop
    Process::_exit_signal = retval;
    Process::_exit_signal_caught = true;
    // Join terminate checker
    _terminate_checker.stop();
    // Exit with normal exit code if terminated or interrupted,
    // with caught signal otherwise
    exit((retval == SIGTERM || retval == SIGINT) ? 0 : retval);
}


void Process::init(int rank, bool leafProcess) {

    _rank = rank;
    _exit_signal_caught = false;
    _main_process = !leafProcess;

    _terminate_checker.run([]() {

        while (_terminate_checker.continueRunning() && !_exit_signal_caught) {
            usleep(1000 * 100); // 0.1s
        }

        if (_exit_signal_caught) {
            if (_main_process) forwardTerminateToChildren();

            if (_exit_signal == SIGABRT || _exit_signal == SIGSEGV) {
                int sig = _exit_signal;
                log(V0_CRIT, "[ERROR] pid=%ld tid=%ld signal=%d\n", 
                        Proc::getPid(), Proc::getTid(), sig);
            }
        }
    });

    signal(SIGUSR1, doNothing); // override default action (exit) on SIGUSR1
    signal(SIGSEGV, handleSignal);
    signal(SIGABRT, handleSignal);
    signal(SIGTERM, handleSignal);
    signal(SIGINT,  handleSignal);
}

pid_t Process::createChild() {
    pid_t res = fork();

    if (res > 0) {
        // parent process
        auto lock = _children_mutex.getLock();
        _children.insert(res);
    } else if (res == -1) {
        // fork failed
        log(V0_CRIT, "[ERROR] fork failed, errno %i\n", (int)errno);
        abort();
    }

    return res;
}

void Process::terminate(pid_t childpid) {
    sendSignal(childpid, SIGTERM);
}
void Process::hardkill(pid_t childpid) {
    sendSignal(childpid, SIGKILL);
}
void Process::suspend(pid_t childpid) {
    sendSignal(childpid, SIGTSTP);
}
void Process::resume(pid_t childpid) {
    sendSignal(childpid, SIGCONT);
}
void Process::wakeUp(pid_t childpid) {
    sendSignal(childpid, SIGUSR1);
}

void Process::forwardTerminateToChildren() { 
    // Propagate signal to children
    auto lock = _children_mutex.getLock();
    for (pid_t childpid : Process::_children) {
        terminate(childpid);
        resume(childpid);
    }
}

void Process::sendSignal(pid_t childpid, int signum) {
    int result = kill(childpid, signum);
    if (result == -1) {
        log(V1_WARN, "[WARN] kill -%i %i returned -1\n", signum, childpid);
    }
}

bool Process::didChildExit(pid_t childpid) {

    auto lock = _children_mutex.getLock();
    if (!_children.count(childpid)) return true;
    
    int status;
    pid_t result = waitpid(childpid, &status, WNOHANG /*| WUNTRACED | WCONTINUED*/);
    if (result != 0) {
        _children.erase(childpid);
        return true;
    }
    return false;
}

std::optional<int> Process::isExitSignalCaught() {
    std::optional<int> opt;
    if (_exit_signal_caught) opt = _exit_signal;
    return opt;
}

void Process::writeTrace(long tid) {
    long callingTid = Proc::getTid();
    std::string command = "gdb --q --n --ex bt --batch --pid " + std::to_string(tid) 
            + " > mallob_thread_trace_" + std::to_string(tid) 
            + "_by_" + std::to_string(callingTid) + " 2>&1";
    system(command.c_str());
}

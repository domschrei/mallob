
#ifndef DOMPASCH_MALLOB_TERMINATOR_HPP
#define DOMPASCH_MALLOB_TERMINATOR_HPP

#include <atomic>

#include "util/sys/process.hpp"

class Terminator {

private:
    static std::atomic_bool _exit;

public:
    static void setTerminating() {
        _exit = true;
    }
    static bool isTerminating() {
        if (!_exit && Process::isExitSignalCaught()) setTerminating();
        return _exit;
    }

};

#endif
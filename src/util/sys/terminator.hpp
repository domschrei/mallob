
#ifndef DOMPASCH_MALLOB_TERMINATOR_HPP
#define DOMPASCH_MALLOB_TERMINATOR_HPP

#include <atomic>

class Terminator {

private:
    static std::atomic_bool _exit;

public:
    static void setTerminating() {
        _exit = true;
    }
    static bool isTerminating() {
        return _exit;
    }

};

#endif

#include "terminator.hpp"

#include "comm/mympi.hpp"

std::atomic_bool Terminator::_exit = false;

void Terminator::broadcastExitSignal() {
    MyMpi::isend(0, MSG_DO_EXIT, std::vector<uint8_t>(1, 0));
}

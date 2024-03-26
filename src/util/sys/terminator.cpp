
#include "terminator.hpp"

#include <cstdint>
#include <vector>

#include "comm/mympi.hpp"
#include "comm/msgtags.h"

std::atomic_bool Terminator::_exit = false;

void Terminator::broadcastExitSignal() {
    MyMpi::isend(0, MSG_DO_EXIT, std::vector<uint8_t>(1, 0));
}

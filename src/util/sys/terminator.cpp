
#include "terminator.hpp"

#include <cstdint>
#include <vector>

#include "comm/mympi.hpp"
#include "comm/msgtags.h"
#include "comm/msg_queue/message_queue.hpp"
#include "util/sys/proc.hpp"

std::atomic_bool Terminator::_exit = false;

void Terminator::broadcastExitSignal() {

    auto cmd = "bash scripts/kill-delayed.sh " + std::to_string(Proc::getPid());
    system(cmd.c_str());

    MyMpi::isend(0, MSG_DO_EXIT, std::vector<uint8_t>(1, 0));
    do {
        MyMpi::getMessageQueue().advance();
    } while (MyMpi::getMessageQueue().hasOpenRecvFragments() || MyMpi::getMessageQueue().hasOpenSends());
}

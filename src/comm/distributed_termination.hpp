
#pragma once

#include "comm/msg_queue/message_subscription.hpp"
#include "comm/mympi.hpp"
#include "mpi.h"

class DistributedTermination {

private:
    MessageSubscription _subscription;
    bool _triggered {false};

    enum MsgType {UP, DOWN};

public:
    DistributedTermination() :
        _subscription(MSG_DO_EXIT, [&](MessageHandle& h) {
            if (h.getRecvData()[0] == UP) trigger();
            if (h.getRecvData()[0] == DOWN) downwards();
        }) {}

    bool triggered() const {return _triggered;}

    void trigger() {
        if (_triggered) return;
        _triggered = true;
        if (MyMpi::rank(MPI_COMM_WORLD) == 0) {
            // broadcast
            downwards();
        } else {
            upwards();
        }
    }

private:
    void upwards() {
        int parent = (MyMpi::rank(MPI_COMM_WORLD)-1) / 2;
        MyMpi::isend(parent, MSG_DO_EXIT, std::vector<uint8_t>(1, UP));
    }

    void downwards() {
        _triggered = true;
        for (int child : {2*MyMpi::rank(MPI_COMM_WORLD)+1, 2*MyMpi::rank(MPI_COMM_WORLD)+2}) {
            if (child < MyMpi::size(MPI_COMM_WORLD)) {
                MyMpi::isend(child, MSG_DO_EXIT, std::vector<uint8_t>(1, DOWN));
            }
        }
    }
};

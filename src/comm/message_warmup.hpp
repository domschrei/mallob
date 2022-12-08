
#pragma once

#include "mympi.hpp"
#include "msg_queue/message_subscription.hpp"
#include "data/job_transfer.hpp"

class MessageWarmup {

private:
    MPI_Comm& _comm;

    std::list<MessageSubscription> _subscriptions;
    std::vector<int> _hop_destinations;

    MPI_Request _req_barrier;

public:
    MessageWarmup(MPI_Comm& comm, std::vector<int> hopDestinations) :
            _comm(comm), _hop_destinations(hopDestinations) {

        _subscriptions.emplace_back(MSG_WARMUP, [&](auto& h) {
            LOG_ADD_SRC(V4_VVER, "Received warmup msg", h.source);
        });
    }

    void performWarmup() {
        
        // Send warmup messages
        IntVec payload({1, 2, 3, 4, 5, 6, 7, 8});
        for (auto rank : _hop_destinations) {
            MyMpi::isend(rank, MSG_WARMUP, payload);
            LOG_ADD_DEST(V4_VVER, "Sending warmup msg", rank);
            MyMpi::getMessageQueue().advance();
        }

        // Pass barrier
        MPI_Ibarrier(_comm, &_req_barrier);

        // Wait until everyone passed the barrier
        while (true) {
            int flag;
            MPI_Test(&_req_barrier, &flag, MPI_STATUS_IGNORE);
            if (flag) {
                break;
            }
            MyMpi::getMessageQueue().advance();
        }
    }
};

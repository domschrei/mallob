
#include <cmath>

#include "reduceable.h"
#include "util/console.h"


bool Reduceable::startReduction(MPI_Comm& comm) {
    Console::log(Console::VERB, "Starting reduction");
    this->comm = comm;
    excludedRanks.clear();
    myRank = MyMpi::rank(comm);
    highestPower = 2 << (int)std::ceil(std::log2(MyMpi::size(comm)));

    for (power = 2; power <= highestPower; power *= 2) {
        if (myRank % power == 0 && myRank+power/2 < MyMpi::size(comm)) {
            // Receive
            Console::log(Console::VVVVERB, "Red. k=%i : Receiving", power);
            MyMpi::irecv(comm, myRank+power/2, MSG_COLLECTIVES);
            return false;
        } else if (myRank % power == power/2) {
            // Send
            Console::log_send(Console::VVVVERB, myRank-power/2, "Red. k=%i : Sending", power);
            MyMpi::isend(comm, myRank-power/2, MSG_COLLECTIVES, *this);
        }
    }

    // already finished (sending / receiving nothing)
    if (isEmpty()) {
        Console::log(Console::VVVVERB, "Red. : Will not participate in broadcast");
        excludedRanks.insert(myRank);
        Console::log(Console::VVVVERB, "Red. : %i excluded ranks", excludedRanks.size());
    }
    return true; 
}

bool Reduceable::advanceReduction(MessageHandlePtr handle) {

    std::unique_ptr<Reduceable> received = getDeserialized(*handle->recvData);
    if (received->isEmpty()) {
        int source = handle->source;
        excludedRanks.insert(source);
        Console::log(Console::VVVVERB, "-- empty!");
    }
    merge(*received); // reduce into local object

    power *= 2;
    while (power <= highestPower) {
        if (myRank % power == 0 && myRank+power/2 < MyMpi::size(comm)) {
            // Receive
            Console::log(Console::VVVVERB, "Red. k=%i : Receiving", power);
            MyMpi::irecv(comm, myRank+power/2, MSG_COLLECTIVES);
            return false;
        } else if (myRank % power == power/2) {
            // Send
            Console::log_send(Console::VVVVERB, myRank-power/2, "Red. k=%i : Sending", power);
            MyMpi::isend(comm, myRank-power/2, MSG_COLLECTIVES, *this);
        }
        power *= 2;
    }

    // Finished!
    if (isEmpty()) {
        Console::log(Console::VVVVERB, "Red. : Will not participate in broadcast");
        excludedRanks.insert(myRank);
    }
    return true; // finished
}

bool Reduceable::startBroadcast(MPI_Comm& comm, std::set<int>& excludedRanks) {
    Console::log(Console::VVVVERB, "Starting broadcast");

    this->comm = comm;
    myRank = MyMpi::rank(comm);
    highestPower = 2 << (int)std::ceil(std::log2(MyMpi::size(comm)));
    this->excludedRanks = excludedRanks;

    if (excludedRanks.count(myRank)) {
        Console::log(Console::VVVVERB, "Brc. : Not participating");
        return true;
    }

    for (power = highestPower; power >= 2; power /= 2) {
        if (myRank % power == 0 && myRank+power/2 < MyMpi::size(comm)) {
            // Send
            if (excludedRanks.count(myRank+power/2)) {
                continue;
            }
            Console::log_send(Console::VVVVERB, myRank+power/2, "Brc. k=%i : Sending", power);
            MyMpi::isend(comm, myRank+power/2, MSG_COLLECTIVES, *this);

        } else if (myRank % power == power/2) {
            // Receive
            Console::log(Console::VVVVERB, "Brc. k=%i : Receiving", power);
            MyMpi::irecv(comm, myRank-power/2, MSG_COLLECTIVES);
            return false;
        }
    }
    return true;
}

bool Reduceable::advanceBroadcast(MessageHandlePtr handle) {
    deserialize(*handle->recvData); // overwrite local data

    power /= 2;
    for (; power >= 2; power /= 2) {

        if (myRank % power == 0 && myRank+power/2 < MyMpi::size(comm)) {
            // Send
            if (excludedRanks.count(myRank+power/2)) {
                continue;
            }
            Console::log_send(Console::VVVVERB, myRank+power/2, "Brc. k=%i : Sending", power);
            MyMpi::isend(comm, myRank+power/2, MSG_COLLECTIVES, *this);

        } else if (myRank % power == power/2) {
            // Receive
            Console::log(Console::VVVVERB, "Brc. k=%i : Receiving", power);
            MyMpi::irecv(comm, myRank-power/2, MSG_COLLECTIVES);
            return false;
        }
    }

    return true;
}
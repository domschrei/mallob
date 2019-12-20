
#include <cmath>

#include "reduceable.h"
#include "util/console.h"

std::set<int> Reduceable::allReduce(MPI_Comm& comm) {

    // Reduce information
    std::set<int> excludedNodes = reduceToRankZero(comm);
    // Broadcast information
    broadcastFromRankZero(comm, excludedNodes);

    return excludedNodes;
}

std::set<int> Reduceable::reduceToRankZero(MPI_Comm& comm) {
    int myRank = MyMpi::rank(comm);
    int highestPower = 2 << (int)std::ceil(std::log2(MyMpi::size(comm)));
    std::set<int> excludedNodes;
    for (int k = 2; k <= highestPower; k *= 2) {
        if (myRank % k == 0 && myRank+k/2 < MyMpi::size(comm)) {
            // Receive
            Console::log(Console::VVVERB, "Red. k=%i : Receiving", k);
            MessageHandlePtr handle = MyMpi::recv(comm, MSG_COLLECTIVES);
            std::unique_ptr<Reduceable> received = getDeserialized(*handle->recvData);
            if (received->isEmpty()) {
                excludedNodes.insert(handle->source);
                Console::log(Console::VVVERB, "-- empty!");
            }
            merge(*received); // reduce into local object
        } else if (myRank % k == k/2) {
            // Send
            Console::log_send(Console::VVVERB, myRank-k/2, "Red. k=%i : Sending", k);
            MessageHandlePtr handle = MyMpi::send(comm, myRank-k/2, MSG_COLLECTIVES, *this);
        }
    }
    if (isEmpty()) {
        Console::log(Console::VVVERB, "Red. : Will not participate in broadcast");
        excludedNodes.insert(myRank);
    }
    return excludedNodes;
}

void Reduceable::broadcastFromRankZero(MPI_Comm& comm, std::set<int> excludedRanks) {
    int myRank = MyMpi::rank(comm);
    if (excludedRanks.count(myRank))
        return;
    int highestPower = 2 << (int)std::ceil(std::log2(MyMpi::size(comm)));
    for (int k = highestPower; k >= 2; k /= 2) {
        if (myRank % k == 0 && myRank+k/2 < MyMpi::size(comm)) {
            // Send
            if (excludedRanks.count(myRank+k/2)) {
                continue;
            }
            Console::log_send(Console::VVVERB, myRank+k/2, "Brc. k=%i : Sending", k);
            MessageHandlePtr handle = MyMpi::send(comm, myRank+k/2, MSG_COLLECTIVES, *this);
        } else if (myRank % k == k/2) {
            // Receive
            Console::log(Console::VVVERB, "Brc. k=%i : Receiving", k);
            MessageHandlePtr handle = MyMpi::recv(comm, MSG_COLLECTIVES);
            deserialize(*handle->recvData); // overwrite local object
        }
    }
}












bool Reduceable::startReduction(MPI_Comm& comm) {
    Console::log(Console::VERB, "Starting reduction");
    this->comm = comm;
    excludedRanks.clear();
    myRank = MyMpi::rank(comm);
    highestPower = 2 << (int)std::ceil(std::log2(MyMpi::size(comm)));

    for (power = 2; power <= highestPower; power *= 2) {
        if (myRank % power == 0 && myRank+power/2 < MyMpi::size(comm)) {
            // Receive
            Console::log(Console::VVVERB, "Red. k=%i : Receiving", power);
            MyMpi::irecv(comm, myRank+power/2, MSG_COLLECTIVES);
            return false;
        } else if (myRank % power == power/2) {
            // Send
            Console::log_send(Console::VVVERB, myRank-power/2, "Red. k=%i : Sending", power);
            MyMpi::isend(comm, myRank-power/2, MSG_COLLECTIVES, *this);
        }
    }

    // already finished (sending / receiving nothing)
    if (isEmpty()) {
        Console::log(Console::VVVERB, "Red. : Will not participate in broadcast");
        excludedRanks.insert(myRank);
        Console::log(Console::VVVERB, "Red. : %i excluded ranks", excludedRanks.size());
    }
    return true; 
}

bool Reduceable::advanceReduction(MessageHandlePtr handle) {

    std::unique_ptr<Reduceable> received = getDeserialized(*handle->recvData);
    if (received->isEmpty()) {
        int source = handle->source;
        excludedRanks.insert(source);
        Console::log(Console::VVVERB, "-- empty!");
    }
    merge(*received); // reduce into local object

    power *= 2;
    while (power <= highestPower) {
        if (myRank % power == 0 && myRank+power/2 < MyMpi::size(comm)) {
            // Receive
            Console::log(Console::VVVERB, "Red. k=%i : Receiving", power);
            MyMpi::irecv(comm, myRank+power/2, MSG_COLLECTIVES);
            return false;
        } else if (myRank % power == power/2) {
            // Send
            Console::log_send(Console::VVVERB, myRank-power/2, "Red. k=%i : Sending", power);
            MyMpi::isend(comm, myRank-power/2, MSG_COLLECTIVES, *this);
        }
        power *= 2;
    }

    // Finished!
    if (isEmpty()) {
        Console::log(Console::VVVERB, "Red. : Will not participate in broadcast");
        excludedRanks.insert(myRank);
    }
    return true; // finished
}

bool Reduceable::startBroadcast(MPI_Comm& comm, std::set<int>& excludedRanks) {
    Console::log(Console::VVVERB, "Starting broadcast");

    this->comm = comm;
    myRank = MyMpi::rank(comm);
    highestPower = 2 << (int)std::ceil(std::log2(MyMpi::size(comm)));
    this->excludedRanks = excludedRanks;

    if (excludedRanks.count(myRank)) {
        Console::log(Console::VVVERB, "Brc. : Not participating");
        return true;
    }

    for (power = highestPower; power >= 2; power /= 2) {
        if (myRank % power == 0 && myRank+power/2 < MyMpi::size(comm)) {
            // Send
            if (excludedRanks.count(myRank+power/2)) {
                continue;
            }
            Console::log_send(Console::VVVERB, myRank+power/2, "Brc. k=%i : Sending", power);
            MyMpi::isend(comm, myRank+power/2, MSG_COLLECTIVES, *this);

        } else if (myRank % power == power/2) {
            // Receive
            Console::log(Console::VVVERB, "Brc. k=%i : Receiving", power);
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
            Console::log_send(Console::VVVERB, myRank+power/2, "Brc. k=%i : Sending", power);
            MyMpi::isend(comm, myRank+power/2, MSG_COLLECTIVES, *this);

        } else if (myRank % power == power/2) {
            // Receive
            Console::log(Console::VVVERB, "Brc. k=%i : Receiving", power);
            MyMpi::irecv(comm, myRank-power/2, MSG_COLLECTIVES);
            return false;
        }
    }

    return true;
}
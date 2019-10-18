
#include <cmath>

#include "reduceable.h"
#include "util/console.h"

std::set<int> Reduceable::allReduce(MPI_Comm& comm) {

    // Reduce information
    std::set<int> excludedNodes = reduceToRankZero(comm);
    // Broadcast information
    broadcastFromRankZero(comm, excludedNodes);

    MPI_Barrier(comm);

    return excludedNodes;
}

std::set<int> Reduceable::reduceToRankZero(MPI_Comm& comm) {
    int myRank = MyMpi::rank(comm);
    int highestPower = 2 << (int)std::ceil(std::log2(MyMpi::size(comm)));
    std::set<int> excludedNodes;
    for (int k = 2; k <= highestPower; k *= 2) {
        if (myRank % k == 0 && myRank+k/2 < MyMpi::size(comm)) {
            // Receive
            Console::log(Console::VVERB, "Red. k=%i : Receiving", k);
            MessageHandlePtr handle = MyMpi::recv(comm, MSG_REDUCE_RESOURCES_INFO);
            std::unique_ptr<Reduceable> received = getDeserialized(handle->recvData);
            if (received->isEmpty()) {
                excludedNodes.insert(handle->source);
                Console::log(Console::VVERB, "-- empty!");
            }
            merge(*received); // reduce into local object
        } else if (myRank % k == k/2) {
            // Send
            Console::log_send(Console::VVERB, myRank-k/2, "Red. k=%i : Sending", k);
            MessageHandlePtr handle = MyMpi::send(comm, myRank-k/2, MSG_REDUCE_RESOURCES_INFO, *this);
        }
    }
    if (isEmpty()) {
        Console::log(Console::VVERB, "Red. : Will not participate in broadcast");
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
            Console::log_send(Console::VVERB, myRank+k/2, "Brc. k=%i : Sending", k);
            MessageHandlePtr handle = MyMpi::send(comm, myRank+k/2, MSG_REDUCE_RESOURCES_INFO, *this);
        } else if (myRank % k == k/2) {
            // Receive
            Console::log(Console::VVERB, "Brc. k=%i : Receiving", k);
            MessageHandlePtr handle = MyMpi::recv(comm, MSG_REDUCE_RESOURCES_INFO);
            deserialize(handle->recvData); // overwrite local object
        }
    }
}

#include <cmath>

#include "reduceable.hpp"
#include "util/console.hpp"


bool Reduceable::startReduction(MPI_Comm& comm, std::set<int> excludedRanks) {
    Console::log(Console::VVVVERB, "Starting reduction");
    _comm = comm;
    _excluded_ranks = excludedRanks;
    _my_rank = MyMpi::rank(comm);
    if (_excluded_ranks.count(_my_rank)) {
        return true; // not participating -- finished
    }
    _highest_power = 2 << (int)std::ceil(std::log2(MyMpi::size(comm)));

    for (_power = 2; _power <= _highest_power; _power *= 2) {
        if (_my_rank % _power == 0 && _my_rank+_power/2 < MyMpi::size(comm)) {
            // Receive
            if (_excluded_ranks.count(_my_rank+_power/2)) continue;
            Console::log(Console::VVVVERB, "Red. k=%i : Receiving", _power);
            MyMpi::irecv(comm, _my_rank+_power/2, MSG_COLLECTIVE_OPERATION);
            return false;
        } else if (_my_rank % _power == _power/2) {
            // Send
            if (_excluded_ranks.count(_my_rank-_power/2)) continue;
            Console::log_send(Console::VVVVERB, _my_rank-_power/2, "Red. k=%i : Sending", _power);
            MyMpi::isend(comm, _my_rank-_power/2, MSG_COLLECTIVE_OPERATION, *this);
        }
    }

    // already finished (sending / receiving nothing)
    if (isEmpty()) {
        Console::log(Console::VVVVERB, "Red. : Will not participate in broadcast");
        _excluded_ranks.insert(_my_rank);
        Console::log(Console::VVVVERB, "Red. : %i excluded ranks", _excluded_ranks.size());
    }
    return true; 
}

bool Reduceable::advanceReduction(MessageHandle& handle) {

    std::unique_ptr<Reduceable> received = getDeserialized(handle.recvData);
    if (received->isEmpty()) {
        int source = handle.source;
        _excluded_ranks.insert(source);
        Console::log(Console::VVVVERB, "-- empty!");
    }
    merge(*received); // reduce into local object

    _power *= 2;
    while (_power <= _highest_power) {
        if (_my_rank % _power == 0 && _my_rank+_power/2 < MyMpi::size(_comm)) {
            // Receive
            if (_excluded_ranks.count(_my_rank+_power/2) == 0) {
                Console::log(Console::VVVVERB, "Red. k=%i : Receiving", _power);
                MyMpi::irecv(_comm, _my_rank+_power/2, MSG_COLLECTIVE_OPERATION);
                return false;
            } 
        } else if (_my_rank % _power == _power/2) {
            // Send
            if (_excluded_ranks.count(_my_rank-_power/2) == 0) {
                Console::log_send(Console::VVVVERB, _my_rank-_power/2, "Red. k=%i : Sending", _power);
                MyMpi::isend(_comm, _my_rank-_power/2, MSG_COLLECTIVE_OPERATION, *this);
            }
        }
        _power *= 2;
    }

    // Finished!
    if (isEmpty()) {
        Console::log(Console::VVVVERB, "Red. : Will not participate in broadcast");
        _excluded_ranks.insert(_my_rank);
    }
    return true; // finished
}

bool Reduceable::startBroadcast(MPI_Comm& comm, std::set<int>& excludedRanks) {
    Console::log(Console::VVVVERB, "Starting broadcast");

    this->_comm = comm;
    _my_rank = MyMpi::rank(comm);
    _highest_power = 2 << (int)std::ceil(std::log2(MyMpi::size(comm)));
    this->_excluded_ranks = excludedRanks;

    if (excludedRanks.count(_my_rank)) {
        Console::log(Console::VVVVERB, "Brc. : Not participating");
        return true;
    }

    for (_power = _highest_power; _power >= 2; _power /= 2) {
        if (_my_rank % _power == 0 && _my_rank+_power/2 < MyMpi::size(comm)) {
            // Send
            if (excludedRanks.count(_my_rank+_power/2)) {
                continue;
            }
            Console::log_send(Console::VVVVERB, _my_rank+_power/2, "Brc. k=%i : Sending", _power);
            MyMpi::isend(comm, _my_rank+_power/2, MSG_COLLECTIVE_OPERATION, *this);

        } else if (_my_rank % _power == _power/2) {
            // Receive
            Console::log(Console::VVVVERB, "Brc. k=%i : Receiving", _power);
            MyMpi::irecv(comm, _my_rank-_power/2, MSG_COLLECTIVE_OPERATION);
            return false;
        }
    }
    return true;
}

bool Reduceable::advanceBroadcast(MessageHandle& handle) {
    deserialize(handle.recvData); // overwrite local data

    _power /= 2;
    for (; _power >= 2; _power /= 2) {

        if (_my_rank % _power == 0 && _my_rank+_power/2 < MyMpi::size(_comm)) {
            // Send
            if (_excluded_ranks.count(_my_rank+_power/2)) {
                continue;
            }
            Console::log_send(Console::VVVVERB, _my_rank+_power/2, "Brc. k=%i : Sending", _power);
            MyMpi::isend(_comm, _my_rank+_power/2, MSG_COLLECTIVE_OPERATION, *this);

        } else if (_my_rank % _power == _power/2) {
            // Receive
            Console::log(Console::VVVVERB, "Brc. k=%i : Receiving", _power);
            MyMpi::irecv(_comm, _my_rank-_power/2, MSG_COLLECTIVE_OPERATION);
            return false;
        }
    }

    return true;
}


#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>

#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "data/job_transfer.hpp"
#include "comm/msg_queue/message_subscription.hpp"

const int TAG_INT_VEC = 111;
const int TAG_ACK = 112;
const int TAG_EXIT = 113;
const int TAG_PINGPONG = 114;

void testSelfMessages() {

    Terminator::reset();
    int rank = MyMpi::rank(MPI_COMM_WORLD);
    auto& q = MyMpi::getMessageQueue();
    
    int numReceived = 0;
    int sumReceived = 0;
    MessageSubscription sub(TAG_INT_VEC, [&](MessageHandle& h) {
        numReceived++;
        IntVec vec = Serializable::get<IntVec>(h.getRecvData());
        for (int x : vec.data) sumReceived += x; 
    });

    for (size_t i = 0; i < 100; i++) {
        IntVec v;
        v.data.push_back(1);
        MyMpi::isend(rank, TAG_INT_VEC, v);
    }

    while (!Terminator::isTerminating() && 
            (sumReceived != 100 || numReceived != 100))
        q.advance();
}

void testSimpleP2P() {

    Terminator::reset();
    int rank = MyMpi::rank(MPI_COMM_WORLD);
    auto& q = MyMpi::getMessageQueue();

    int totalSum = 0;
    int numReceived = 0;
    MessageSubscription sub(TAG_INT_VEC, [&](MessageHandle& h) {
        LOG(V2_INFO, "received ...\n");
        auto vec = Serializable::get<IntVec>(h.getRecvData()).data;
        size_t sum = 0; for (int x : vec) sum += x;
        LOG(V2_INFO, "Received %i ints, sum: %i\n", vec.size(), sum);
        totalSum += sum;
        numReceived++;
        if (totalSum == 900 && numReceived == 100) {
            LOG(V2_INFO, "All correct, bye\n");
            Terminator::setTerminating();
        }
    });

    for (int i = 0; i < 5000; i++) {
        if (rank == 0) {
            IntVec vec;
            vec.data.push_back(1);
            vec.data.push_back(2);
            vec.data.push_back(3);
            vec.data.push_back(3);
            MyMpi::isend(1, TAG_INT_VEC, vec);
            LOG(V2_INFO, "sent ...\n");
        } else {
            IntVec vec;
            vec.data.push_back(4);
            vec.data.push_back(5);
            MyMpi::isend(0, TAG_INT_VEC, vec);
            LOG(V2_INFO, "sent ...\n");
        }
    }

    while (!Terminator::isTerminating()) q.advance();
}

void testBigP2P() {

    Terminator::reset();

    const bool verifyData = true;
    const int N[] = {10, 249999, 250000, 250001, 900000, 1249999, 1250000, 1250001, 100000000};
    //const int N[] = {10, 249999, 250000, 250001, 900000};
    const int numTests = sizeof(N) / sizeof(int);
    std::vector<std::vector<uint8_t>> vectors;
    for (size_t j = 0; j < numTests; j++) {
        IntVec vec;
        for (int i = 0; i < N[j]; i++) {
            vec.data.push_back(i);
        }
        vectors.push_back(vec.serialize());
    }

    int rank = MyMpi::rank(MPI_COMM_WORLD);
    auto& q = MyMpi::getMessageQueue();
    size_t msgIdx = 0;

    auto sendNextVec = [&]() {
        MyMpi::isend(1-rank, TAG_INT_VEC, std::move(vectors[msgIdx]));
        LOG(V2_INFO, "#%i sent (n=%i)\n", msgIdx, N[msgIdx]);
    };

    MessageSubscription sub1(TAG_INT_VEC, [&](MessageHandle& h) {
        LOG(V2_INFO, "#%i received\n", msgIdx);
        
        if (verifyData) {
            LOG(V2_INFO, "#%i verifying ...\n", msgIdx);
            assert(h.tag == TAG_INT_VEC);
            auto vec = Serializable::get<IntVec>(h.getRecvData()).data;
            assert(vec.size() == N[msgIdx] || LOG_RETURN_FALSE("Wrong size: %i != %i\n", vec.size(), N[msgIdx]));
            for (size_t i = 0; i < vec.size(); i++) {
                assert(vec[i] == i || LOG_RETURN_FALSE("Data at pos. %i: %i\n", i, vec[i]));
            }
            LOG(V2_INFO, "#%i verified\n", msgIdx);
        }
        
        msgIdx++;
        MyMpi::isend(h.source, TAG_ACK, IntVec());
    });
    MessageSubscription sub2(TAG_ACK, [&](MessageHandle& h) {
        msgIdx++;
        if (msgIdx == numTests) {
            MyMpi::isend(1-rank, TAG_EXIT, IntVec());
            MyMpi::isend(rank, TAG_EXIT, IntVec());
        } else {
            sendNextVec();
        }
    });
    MessageSubscription sub3(TAG_EXIT, [&](MessageHandle& h) {
        Terminator::setTerminating();
    });

    MPI_Barrier(MPI_COMM_WORLD);
    
    float maxDelay = 0;
    float lastPing = Timer::elapsedSeconds();
    MessageSubscription sub4(TAG_PINGPONG, [&](MessageHandle& h) {
        MyMpi::isend(1-rank, TAG_PINGPONG, IntVec());
        float time = Timer::elapsedSeconds();
        if (time - lastPing > maxDelay) {
            maxDelay = time - lastPing;
            LOG(V2_INFO, "New max. delay %.4fs\n", maxDelay);
        }
        lastPing = time;
    });

    if (rank == 0) {
        //MyMpi::isend(1-rank, TAG_PINGPONG, IntVec());
        sendNextVec();
    }

    while (!Terminator::isTerminating()) {
        q.advance();
        float time = Timer::elapsedSeconds();
        if (time - lastPing > maxDelay) {
            maxDelay = time - lastPing;
            LOG(V2_INFO, "New max. delay %.4fs\n", maxDelay);
        }
        lastPing = time;
    }

    LOG(V2_INFO, "Max delay: %.4f s\n", maxDelay);
}

int main(int argc, char *argv[]) {

    MyMpi::init();
    Timer::init();
    int rank = MyMpi::rank(MPI_COMM_WORLD);

    Process::init(rank);

    Random::init(rand(), rand());
    Logger::init(rank, V5_DEBG);

    Parameters params;
    params.init(argc, argv);
    MyMpi::setOptions(params);

    //testSelfMessages();
    //testSimpleP2P();
    testBigP2P();

    MPI_Finalize();
}
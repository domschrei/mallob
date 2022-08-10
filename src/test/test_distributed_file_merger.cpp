
#include <iostream>
#include "util/assert.hpp"
#include <vector>
#include <string>

#include "util/random.hpp"
#include "util/sat_reader.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"
#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "comm/distributed_file_merger.hpp"
#include "util/sys/watchdog.hpp"

void testMerge(int myRank) {

    int lineCounter = 0;
    int maxLineCounter = 1000000;

    auto merger = DistributedFileMerger(MPI_COMM_WORLD, 5, [&]() {
        if (lineCounter == maxLineCounter) 
            return std::optional<DistributedFileMerger::IdQualifiedLine>();
        DistributedFileMerger::IdQualifiedLine line;
        line.id = MyMpi::size(MPI_COMM_WORLD)*(maxLineCounter - lineCounter) + myRank;
        line.body = std::to_string(lineCounter) + "th line from rank " + std::to_string(myRank) + "\n";
        lineCounter++;
        return std::optional<DistributedFileMerger::IdQualifiedLine>(line);
    }, "final_output.txt");

    MyMpi::getMessageQueue().registerCallback(MSG_ADVANCE_DISTRIBUTED_FILE_MERGE, [&](MessageHandle& h) {
        DistributedFileMerger::MergeMessage msg; msg.deserialize(h.getRecvData());
        merger.handle(h.source, msg);
    });

    while (!merger.readyToMerge());
    merger.beginMerge();

    Watchdog watchdog(true, 1000, Timer::elapsedSeconds());
    while (true) {
        MyMpi::getMessageQueue().advance();
        merger.advance();
        if (merger.finished()) break;
        watchdog.reset(Timer::elapsedSeconds());
    }
    LOG(V2_INFO, "Done, exiting\n");
}

int main(int argc, char *argv[]) {

    MyMpi::init();
    Timer::init();
    int rank = MyMpi::rank(MPI_COMM_WORLD);

    Process::init(rank);

    Random::init(rand(), rand());
    Logger::init(rank, V5_DEBG, false, false, false, nullptr);

    Parameters params;
    params.init(argc, argv);
    MyMpi::setOptions(params);

    ProcessWideThreadPool::init(4);

    testMerge(rank);

    MPI_Finalize();
}
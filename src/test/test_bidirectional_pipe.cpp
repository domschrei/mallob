
#include <bits/std_abs.h>
#include <stdlib.h>
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "util/sys/bidirectional_anytime_pipe.hpp"
#include "util/sys/bidirectional_pipe.hpp"
#include "util/assert.hpp"
#include "util/params.hpp"
#include "util/sys/fileutils.hpp"
#include "util/sys/process.hpp"
#include "util/sys/shared_memory.hpp"
#include "util/sys/subprocess.hpp"
#include "util/sys/thread_pool.hpp"
#include "util/random.hpp"
#include "util/logger.hpp"
#include "util/sys/timer.hpp"

const auto pathParentToChild = ".partochld.pipe";
const auto pathChildToParent = ".chldtopar.pipe";
const auto pathParentToChildAnytime = ".partochld.anytime.pipe";
const auto pathChildToParentAnytime = ".chldtopar.anytime.pipe";

const char TAG_SEND_DATA = 's';

void testBasicChild() {

    BiDirectionalPipe pipe(BiDirectionalPipe::ACCESS, pathChildToParent, pathParentToChild);
    pipe.open();

    LOG(V2_INFO, "[child]  wait for data ...\n");
    char tag = TAG_SEND_DATA;
    while (pipe.pollForData() != tag) {}
    LOG(V2_INFO, "[child]  data present\n");
    std::vector<int> data = pipe.readData(tag, true);
    LOG(V2_INFO, "[child]  read all data\n");
    for (size_t i = 0; i < data.size(); i++) data[i]++;
    LOG(V2_INFO, "[child]  transformed data\n");
    LOG(V2_INFO, "[child]  writing data ...\n");
    pipe.writeData(data, TAG_SEND_DATA);
    LOG(V2_INFO, "[child]  wrote all data\n");

    ::exit(0);
}

void testBasic() {

    FileUtils::rm(pathParentToChild);
    FileUtils::rm(pathChildToParent);

    BiDirectionalPipe pipe(BiDirectionalPipe::CREATE, pathParentToChild, pathChildToParent);

    int res = Process::createChild();
    if (res == 0) {
        // [child process]
        testBasicChild(); // does not return
    }

    // [parent process]
    pid_t pid = res;
    pipe.open();
    // assuming 2^24 (≈ 16M) unit clauses with one literal and six ints worth of metadata each
    // -> amounts to around 470MB of data
    const size_t dataLength = 7 * (1<<24);
    std::vector<int> data(dataLength, 7);
    LOG(V2_INFO, "[parent] writing data ...\n");
    pipe.writeData(data, TAG_SEND_DATA);
    LOG(V2_INFO, "[parent] wrote all data\n");
    char tag = TAG_SEND_DATA;
    while (pipe.pollForData() != tag) {}
    LOG(V2_INFO, "[parent] data present\n");
    data = pipe.readData(tag, true);
    LOG(V2_INFO, "[parent] read all data\n");
    assert(data.size() == dataLength);
    assert(data[0] == 8);
    assert(data[1337] == 8);
    assert(data.back() == 8);

    while (!Process::didChildExit(pid)) usleep(1000 * 1000);
    LOG(V2_INFO, "[parent] child exited\n");

    FileUtils::rm(pathParentToChild);
    FileUtils::rm(pathChildToParent);
}


void testAnytimeChild() {
    {
        bool* childReadyToWrite = (bool*) SharedMemory::access("edu.kit.mallob.test.bidirpipe", 1);
        bool* terminatePipe = (bool*) SharedMemory::access("edu.kit.mallob.test.bidirpipe.close", 1);
        BiDirectionalAnytimePipe pipe(BiDirectionalAnytimePipe::ACCESS, pathChildToParentAnytime, pathParentToChildAnytime, childReadyToWrite, terminatePipe);
        pipe.open();

        LOG(V2_INFO, "[child]  wait for data ...\n");
        char tag = TAG_SEND_DATA;
        while (pipe.pollForData() != tag) {}
        LOG(V2_INFO, "[child]  data present\n");
        std::vector<int> data = pipe.readData(tag);
        LOG(V2_INFO, "[child]  read all data\n");
        for (size_t i = 0; i < data.size(); i++) data[i]++;
        LOG(V2_INFO, "[child]  transformed data\n");
        LOG(V2_INFO, "[child]  writing data ...\n");
        pipe.writeData(data, TAG_SEND_DATA);
        LOG(V2_INFO, "[child]  wrote all data\n");
    }
    ::exit(0);
}

void testAnytime() {

    FileUtils::rm(pathParentToChildAnytime);
    FileUtils::rm(pathChildToParentAnytime);
    FileUtils::rm("/dev/shm/edu.kit.mallob.test.bidirpipe");

    bool* childReadyToWrite = (bool*) SharedMemory::create("edu.kit.mallob.test.bidirpipe", 1);
    bool* terminatePipe = (bool*) SharedMemory::create("edu.kit.mallob.test.bidirpipe.close", 1);
    *childReadyToWrite = false;
    *terminatePipe = false;
    pid_t pid;
    {
        BiDirectionalAnytimePipe pipe(BiDirectionalAnytimePipe::CREATE, pathParentToChildAnytime, pathChildToParentAnytime, childReadyToWrite, terminatePipe);

        int res = Process::createChild();
        if (res == 0) {
            // [child process]
            testAnytimeChild(); // does not return
        }

        // [parent process]
        pid = res;
        pipe.open();
        // assuming 2^24 (≈ 16M) unit clauses with one literal and six ints worth of metadata each
        // -> amounts to around 470MB of data
        const size_t dataLength = 7 * (1<<24);
        std::vector<int> data(dataLength, 7);
        LOG(V2_INFO, "[parent] writing data ...\n");
        pipe.writeData(data, TAG_SEND_DATA);
        LOG(V2_INFO, "[parent] wrote all data\n");
        char tag = TAG_SEND_DATA;
        while (pipe.pollForData() != tag) {}
        LOG(V2_INFO, "[parent] data present\n");
        data = pipe.readData(tag);
        LOG(V2_INFO, "[parent] read all data\n");
        assert(data.size() == dataLength);
        assert(data[0] == 8);
        assert(data[1337] == 8);
        assert(data.back() == 8);
    }

    while (!Process::didChildExit(pid)) usleep(10'000);
    LOG(V2_INFO, "[parent] child exited\n");

    SharedMemory::free("edu.kit.mallob.test.bidirpipe", (char*) &childReadyToWrite, 1);
    FileUtils::rm(pathParentToChildAnytime);
    FileUtils::rm(pathChildToParentAnytime);
    FileUtils::rm("/dev/shm/edu.kit.mallob.test.bidirpipe");
}

int main(int argc, char** argv) {
    Timer::init();
    Parameters params;
    params.init(argc, argv);
    Logger::init(0, params.verbosity());
    Random::init(rand(), rand());
    Process::init(0);
    ProcessWideThreadPool::init(4);

    Timer::init();
    LOG(V2_INFO, "*** BASIC ***\n");
    testBasic();

    Timer::init();
    LOG(V2_INFO, "*** ANYTIME ***\n");
    testAnytime();
}

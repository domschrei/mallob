
#include <bits/std_abs.h>
#include <stdlib.h>
#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "util/sys/bidirectional_anytime_pipe_shmem.hpp"
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

constexpr size_t bufSize = 131072;

const char TAG_HELLO = 'H';
const char TAG_SEND_DATA = 's';

void testAnytimeChild() {
    {
        char* shmem = (char*) SharedMemory::access("edu.kit.iti.mallob.test.bidirpipe", 2*bufSize);
        BiDirectionalAnytimePipeShmem pipe(shmem + bufSize, bufSize, shmem, bufSize, false);

        // Hear hello, say hello
        char tag = TAG_HELLO;
        while (pipe.pollForData() != tag) {}
        auto ignore = pipe.readData(tag);
        assert(ignore.empty());
        pipe.writeData({}, tag);

        LOG(V2_INFO, "[child]  wait for data ...\n");
        tag = TAG_SEND_DATA;
        while (pipe.pollForData() != tag) {}
        LOG(V2_INFO, "[child]  data present\n");
        std::vector<int> data = pipe.readData(tag);
        LOG(V2_INFO, "[child]  read all data (len %lu)\n", data.size());
        for (size_t i = 0; i < data.size(); i++) data[i]++;
        LOG(V2_INFO, "[child]  transformed data\n");
        LOG(V2_INFO, "[child]  writing data ...\n");
        pipe.writeData(std::move(data), TAG_SEND_DATA);
        LOG(V2_INFO, "[child]  wrote all data\n");
        pipe.flush(); // wait until output buffer is empty
    }
    ::exit(0);
}

void testAnytime() {

    pid_t pid;
    char* shmem = (char*) SharedMemory::create("edu.kit.iti.mallob.test.bidirpipe", 2*bufSize);
    {
        BiDirectionalAnytimePipeShmem pipe(shmem, bufSize, shmem + bufSize, bufSize, true);

        int res = Process::createChild();
        if (res == 0) {
            // [child process]
            testAnytimeChild(); // does not return
        }

        // [parent process]
        pid = res;

        // Say hello, hear hello
        char tag = TAG_HELLO;
        pipe.writeData({}, tag);
        while (pipe.pollForData() != tag) {}
        auto ignore = pipe.readData(tag);
        assert(ignore.empty());

        // assuming 2^24 (≈ 16M) unit clauses with one literal and six ints worth of metadata each
        // -> amounts to around 470MB of data
        const size_t dataLength = 7 * (1<<24);
        std::vector<int> data(dataLength);
        for (size_t i = 0; i < data.size(); i++) data[i] = i;
        LOG(V2_INFO, "[parent] writing data (len %lu) ...\n", dataLength);
        pipe.writeData(std::move(data), TAG_SEND_DATA);
        LOG(V2_INFO, "[parent] wrote all data\n");
        tag = TAG_SEND_DATA;
        while (pipe.pollForData() != tag) {}
        LOG(V2_INFO, "[parent] data present\n");
        data = pipe.readData(tag);
        LOG(V2_INFO, "[parent] read all data (len %lu)\n", data.size());
        assert(data.size() == dataLength);
        for (size_t i = 0; i < data.size(); i++) assert(data[i] == i+1);

        while (!Process::didChildExit(pid)) usleep(10'000);
    }

    LOG(V2_INFO, "[parent] child exited\n");
    SharedMemory::free("edu.kit.iti.mallob.test.bidirpipe", shmem, 2*bufSize);
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
    testAnytime();
}

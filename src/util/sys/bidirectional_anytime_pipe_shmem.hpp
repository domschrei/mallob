
#pragma once

#include <bits/types/FILE.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <poll.h>
#include <utility>

#include "util/logger.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"
#include "util/sys/background_worker.hpp"
#include "util/assert.hpp"
#include "util/sys/proc.hpp"

class BiDirectionalAnytimePipeShmem {

private:
    volatile char* _data_out;
    size_t _cap_out;
    volatile char* _data_in;
    size_t _cap_in;
    volatile bool _terminate {false};
    volatile size_t _nb_to_write {0};
    volatile size_t _nb_written {0};

    volatile char* _data_out_left;
    volatile char* _data_out_right;
    volatile char* _data_in_left;
    volatile char* _data_in_right;

    struct InPlaceData {
        volatile bool available;
        volatile size_t size;
        volatile bool toBeContinued;
        volatile char tag;
        static InPlaceData* getMetadata(volatile char* buffer) {return (InPlaceData*) buffer;}
        static std::pair<volatile char*, size_t> getDataBuffer(volatile char* buffer, size_t cap) {
            std::pair<volatile char*, size_t> res = {buffer + sizeof(InPlaceData), cap - sizeof(InPlaceData)};
            res.second -= (res.second % sizeof(int)); // size must be multiple of data type
            return res;
        }
    };

    struct Message {
        char tag {0};
        std::vector<int> userData;
    };

    struct IOTask {
        volatile char* shmemLeft;
        volatile char* shmemRight;
        size_t shmemCap;

        InPlaceData* shmemMeta;
        volatile char* shmemBuf;
        unsigned long shmemSize;

        Message msg;
        bool ongoing;
        bool left = true;
        bool done;
        size_t posInMsg;

        IOTask(volatile char* shmemLeft, volatile char* shmemRight, size_t shmemCap)
            : shmemLeft(shmemLeft), shmemRight(shmemRight), shmemCap(shmemCap) {reset();}
        void reset() {
            ongoing = false;
            done = false;
            msg = {};
            posInMsg = 0;
            // initialize shmem fields by toggling "left" twice ...
            left = !left;
            switchBuffers();
        }
        void continueRead() {
            if (!shmemMeta->available) return;

            ongoing = true;
            msg.tag = shmemMeta->tag;
            assert(shmemMeta->size <= shmemSize ||
                log_return_false("[ERROR] prompted to read %lu bytes into buffer of length %lu!\n", shmemMeta->size, shmemSize));
            size_t oldMsgNbInts = msg.userData.size();
            msg.userData.resize(msg.userData.size() + shmemMeta->size / sizeof(int));
            memcpy(msg.userData.data() + oldMsgNbInts, (char*)shmemBuf, shmemMeta->size);
            bool tbc = shmemMeta->toBeContinued;
            shmemMeta->available = false;

            switchBuffers();

            if (!tbc) {
                done = true;
                ongoing = false;
                return;
            }

        }
        void continueWrite() {
            if (shmemMeta->available) return;

            ongoing = true;
            shmemMeta->tag = msg.tag;
            size_t endInMsg = std::min(posInMsg + shmemSize/sizeof(int), msg.userData.size());
            shmemMeta->size = (endInMsg-posInMsg) * sizeof(int);
            memcpy((char*)shmemBuf, msg.userData.data() + posInMsg, shmemMeta->size);
            bool tbc = (posInMsg < msg.userData.size());
            shmemMeta->toBeContinued = tbc;
            posInMsg += shmemMeta->size / sizeof(int);
            shmemMeta->available = true;

            switchBuffers();

            if (!tbc) {
                done = true;
                ongoing = false;
                return;
            }
        }
        void switchBuffers() {
            left = !left;
            shmemMeta = InPlaceData::getMetadata(left ? shmemLeft : shmemRight);
            auto pair = InPlaceData::getDataBuffer(left ? shmemLeft : shmemRight, shmemCap/2);
            shmemBuf = pair.first; shmemSize = pair.second;
        }
    };

    BackgroundWorker _bg_worker;
    SPSCBlockingRingbuffer<Message> _buf_in;
    SPSCBlockingRingbuffer<Message> _buf_out;

    Message _msg_to_read;

public:
    struct ChannelConfig {
        char* data; // the shared-memory data to use for this channel
        size_t capacity; // the size of the shared-memory data in bytes
    };
    BiDirectionalAnytimePipeShmem(ChannelConfig out, ChannelConfig in, bool parent) :
        _data_out(out.data), _cap_out(out.capacity), _data_in(in.data), _cap_in(in.capacity),
        _buf_in(512), _buf_out(512) {

        // double buffer method
        _data_in_left = _data_in;
        _data_in_right = _data_in + in.capacity/2;
        _data_out_left = _data_out;
        _data_out_right = _data_out + out.capacity/2;

        if (parent) {
            InPlaceData::getMetadata(_data_in_left)->available = false;
            InPlaceData::getMetadata(_data_in_right)->available = false;
            InPlaceData::getMetadata(_data_out_left)->available = false;
            InPlaceData::getMetadata(_data_out_right)->available = false;
        }

        // We only run a single background thread for read and/or write tasks
        // (depending on the configuration). If a thread does both, the internal
        // queries must be non-blocking to guarantee progress in both directions.
        // An exception is made for the case where the SPSC ringbuffer for incoming
        // messages runs full (in which case the user does not fetch messages with
        // appropriate frequency), which *can* cause stagnation.
        _bg_worker.run([&]() {
            Proc::nameThisThread("ShmemPipeIO");
            IOTask readTask(_data_in_left, _data_in_right, _cap_in);
            IOTask writeTask(_data_out_left, _data_out_right, _cap_out);
            while (!_terminate) { // run indefinitely until you should terminate
                readTask.continueRead();
                if (readTask.done) {
                    // message read over pipe: push to incoming messages queue
                    bool success = readTask.msg.tag == 0 || _buf_in.pushBlocking(readTask.msg);
                    if (!success) break;
                    readTask.reset();
                }
                // no outgoing message present yet?
                bool success = writeTask.msg.tag != 0 || _buf_out.pollNonblocking(writeTask.msg);
                if (success && writeTask.msg.tag != 0) {
                    // try to write message to the shmem pipe
                    writeTask.continueWrite();
                    if (writeTask.done) {
                        _nb_written++;
                        writeTask.reset();
                    }
                }
                // if no calls are actively ongoing, we should sleep for a little while
                if (!readTask.ongoing && !writeTask.ongoing) usleep(1000);
            }
        });
    }

    // non-blocking "peek" for available data; non-zero at success
    char pollForData() {
        if (_buf_in.empty()) return 0;
        bool ok = _buf_in.pollNonblocking(_msg_to_read);
        if (!ok) return 0;
        LOG(V5_DEBG, "PIPE read %c\n", _msg_to_read.tag);
        return _msg_to_read.tag;
    }
    // immediately returns the available data prepared via a successful pollForData()
    std::vector<int> readData(char& contentTag) {
        const char expectedTag = contentTag;
        contentTag = _msg_to_read.tag;
        assert(expectedTag == contentTag);
        return std::move(_msg_to_read.userData);
    }

    void terminateAsynchronously() {
        _terminate = true;
    }

    // Send a piece of data, can be blocking and copies the data.
    bool writeData(const std::vector<int>& data, char contentTag) {
        return writeData(std::vector<int>(data), contentTag);
    }
    // Send a piece of data built from concatenating the two provided arrays (for convenience),
    // can be blocking and copies the data.
    bool writeData(const std::vector<int>& data1, const std::vector<int>& data2, char contentTag) {
        return writeData(std::vector<int>(data1), data2, contentTag);
    }
    // Send a piece of data, can be blocking and moves the data.
    bool writeData(std::vector<int>&& data, char contentTag) {
        LOG(V5_DEBG, "PIPE write %c\n", contentTag);
        Message msg {contentTag, std::move(data)};
        bool success = _buf_out.pushBlocking(msg);
        if (success) _nb_to_write++;
        return success;
    }
    // Send a piece of data built from concatenating the two provided arrays (for convenience),
    // can be blocking and moves/copies the data.
    bool writeData(std::vector<int>&& data1, const std::vector<int>& data2, char contentTag) {
        LOG(V5_DEBG, "PIPE write %c\n", contentTag);
        data1.insert(data1.end(), data2.begin(), data2.end());
        Message msg {contentTag, std::move(data1)};
        bool success = _buf_out.pushBlocking(msg);
        if (success) _nb_to_write++;
        return success;
    }

    // If writing happens concurrently, wait until all messages have been fully written.
    // Has no effect otherwise.
    void flush() {
        while (!_terminate && _nb_written < _nb_to_write) usleep(3*1000);
    }

    bool hasSpaceForWriting() {
        return !_buf_out.full();
    }

    ~BiDirectionalAnytimePipeShmem() {
        _terminate = true;
        _buf_in.markExhausted();
        _buf_in.markTerminated();
        _buf_out.markExhausted();
        _buf_out.markTerminated();
    }
};

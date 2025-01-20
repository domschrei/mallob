
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

    bool _out_concurrent;
    bool _in_concurrent;

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

    // Internal method to read a single message, possibly buffered across multiple chunks.
    // Uses the provided buffers, of equal provided size, for double buffering.
    // Can be configured to be blocking or non-blocking initially; however, once reading
    // the initial block of data succeeds, the call always blocks until completion.
    bool readData(volatile char* shmemLeft, volatile char* shmemRight, size_t shmemCap, bool blocking, Message& outMsg) {

        // message always begins in the left buffer
        bool left = true;
        InPlaceData* shmemMeta = InPlaceData::getMetadata(shmemLeft);
        auto [shmemBuf, shmemSize] = InPlaceData::getDataBuffer(shmemLeft, shmemCap/2);

        if (!blocking && !shmemMeta->available) return false;
        while (!shmemMeta->available && !_terminate) usleep(1000);
        while (!_terminate) {
            outMsg.tag = shmemMeta->tag;
            assert(shmemMeta->size <= shmemSize ||
                log_return_false("[ERROR] prompted to read %lu bytes into buffer of length %lu!\n", shmemMeta->size, shmemSize));
            size_t oldMsgNbInts = outMsg.userData.size();
            outMsg.userData.resize(outMsg.userData.size() + shmemMeta->size / sizeof(int));
            memcpy(outMsg.userData.data() + oldMsgNbInts, (char*)shmemBuf, shmemMeta->size);
            bool tbc = shmemMeta->toBeContinued;
            shmemMeta->available = false;

            if (!tbc) break; // done!

            // switch buffers
            left = !left;
            shmemMeta = InPlaceData::getMetadata(left ? shmemLeft : shmemRight);
            auto pair = InPlaceData::getDataBuffer(left ? shmemLeft : shmemRight, shmemCap/2);
            shmemBuf = pair.first; shmemSize = pair.second;

            while (!shmemMeta->available && !_terminate) {} // busy waiting since the other thread is on it
        }
        return true;
    }
    // Internal method to write a single message, possibly buffered across multiple chunks.
    // Uses the provided buffers, of equal provided size, for double buffering.
    // Can be configured to be blocking or non-blocking initially; however, once writing
    // the initial block of data succeeds, the call always blocks until completion.
    bool writeData(volatile char* shmemLeft, volatile char* shmemRight, size_t shmemCap, bool blocking, const Message& inMsg) {
        size_t posInMsg = 0;

        // message always begins in the left buffer
        bool left = true;
        InPlaceData* shmemMeta = InPlaceData::getMetadata(shmemLeft);
        auto [shmemBuf, shmemSize] = InPlaceData::getDataBuffer(shmemLeft, shmemCap/2);

        if (!blocking && shmemMeta->available) return false;
        while (shmemMeta->available && !_terminate) usleep(1000);
        while (!_terminate) {
            shmemMeta->tag = inMsg.tag;
            size_t endInMsg = std::min(posInMsg + shmemSize/sizeof(int), inMsg.userData.size());
            shmemMeta->size = (endInMsg-posInMsg) * sizeof(int);
            memcpy((char*)shmemBuf, inMsg.userData.data() + posInMsg, shmemMeta->size);
            bool tbc = (posInMsg < inMsg.userData.size());
            shmemMeta->toBeContinued = tbc;
            posInMsg += shmemMeta->size / sizeof(int);
            shmemMeta->available = true;

            if (!tbc) break; // done!

            // switch buffers
            left = !left;
            shmemMeta = InPlaceData::getMetadata(left ? shmemLeft : shmemRight);
            auto pair = InPlaceData::getDataBuffer(left ? shmemLeft : shmemRight, shmemCap/2);
            shmemBuf = pair.first; shmemSize = pair.second;

            while (shmemMeta->available && !_terminate) {} // busy waiting since the other thread is on it
        }
        return true;
    }

    BackgroundWorker _bg_worker;
    SPSCBlockingRingbuffer<Message> _buf_in;
    SPSCBlockingRingbuffer<Message> _buf_out;

    Message _msg_to_read;

public:
    struct ChannelConfig {
        char* data; // the shared-memory data to use for this channel
        size_t capacity; // the size of the shared-memory data in bytes
        bool concurrent; // whether to perform reading/writing in a background thread
    };
    BiDirectionalAnytimePipeShmem(ChannelConfig out, ChannelConfig in, bool parent) :
        _data_out(out.data), _cap_out(out.capacity), _data_in(in.data), _cap_in(in.capacity),
        _out_concurrent(out.concurrent), _in_concurrent(in.concurrent),
        _buf_in(64), _buf_out(64) {

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
        if (_in_concurrent || _out_concurrent) _bg_worker.run([&]() {
            Proc::nameThisThread("ShmemPipeIO");
            Message msgToRead;
            Message msgToWrite;
            const bool bothConcurrent = _in_concurrent && _out_concurrent;
            while (!_terminate) { // run indefinitely until you should terminate
                if (_in_concurrent) {
                    // read message from the shmem pipe
                    bool success = readData(_data_in_left, _data_in_right, _cap_in,
                        !bothConcurrent, msgToRead);
                    if (success && msgToRead.tag != 0) {
                        // message read over pipe: push to incoming messages queue
                        success = _buf_in.pushBlocking(msgToRead);
                        if (!success) break;
                    }
                }
                if (_out_concurrent) { // run until terminating
                    // no outgoing message present yet?
                    if (msgToWrite.tag == 0) {
                        // poll message from outgoing messages queue
                        bothConcurrent ? _buf_out.pollNonblocking(msgToWrite) : _buf_out.pollBlocking(msgToWrite);
                    }
                    // outgoing message present by now?
                    if (msgToWrite.tag != 0) {
                        // try to write message to the shmem pipe
                        bool success = writeData(_data_out_left, _data_out_right, _cap_out,
                            !bothConcurrent, msgToWrite);
                        if (success) {
                            _nb_written++;
                            msgToWrite.tag = 0; // reset
                        }
                    }
                }
                // if the calls are all non-blocking, we should sleep for a little while
                if (bothConcurrent) usleep(1000);
            }
        });
    }

    // non-blocking "peek" for available data; non-zero at success
    char pollForData() {
        if (!_in_concurrent) {
            bool success = readData(_data_in_left, _data_in_right, _cap_in,
                false, _msg_to_read);
            if (!success) return 0;
            return _msg_to_read.tag;
        }
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
        if (!_out_concurrent) {
            bool success = writeData(_data_out_left, _data_out_right, _cap_out,
                true, msg);
            return success;
        }
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
        if (!_out_concurrent) {
            bool success = writeData(_data_out_left, _data_out_right, _cap_out,
                true, msg);
            return success;
        }
        bool success = _buf_out.pushBlocking(msg);
        if (success) _nb_to_write++;
        return success;
    }

    // If writing happens concurrently, wait until all messages have been fully written.
    // Has no effect otherwise.
    void flush() {
        if (!_out_concurrent) return;
        while (!_terminate && _nb_written < _nb_to_write) usleep(3*1000);
    }

    ~BiDirectionalAnytimePipeShmem() {
        _terminate = true;
        _buf_in.markExhausted();
        _buf_in.markTerminated();
        _buf_out.markExhausted();
        _buf_out.markTerminated();
    }
};

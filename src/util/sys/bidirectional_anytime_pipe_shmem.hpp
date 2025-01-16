
#pragma once

#include <bits/types/FILE.h>
#include <cstddef>
#include <cstdint>
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

    struct InPlaceData {
        volatile bool available;
        volatile size_t size;
        volatile bool toBeContinued;
        volatile char tag;
        static InPlaceData* getMetadata(volatile char* buffer) {return (InPlaceData*) buffer;}
        static std::pair<volatile char*, size_t> getDataBuffer(volatile char* buffer, size_t cap) {
            return {buffer + sizeof(InPlaceData), cap - sizeof(InPlaceData)};
        }
    };

    struct Message {
        char tag {0};
        std::vector<int> userData;
    };

    Message readData(volatile char* rawData, size_t cap) {
        Message msg;
        InPlaceData& data = *InPlaceData::getMetadata(rawData);
        auto [buf, buflen] = InPlaceData::getDataBuffer(rawData, cap);
        int iteration = 1;

        while (!data.available && !_terminate) usleep(1000);
        while (!_terminate) {
            msg.tag = data.tag;
            size_t oldMsgSize = msg.userData.size();
            msg.userData.resize(msg.userData.size() + data.size / sizeof(int));
            memcpy(msg.userData.data() + oldMsgSize, (char*)buf, data.size);
            bool tbc = data.toBeContinued;
            data.available = false;

            if (!tbc) break; // done!
            while (!data.available && !_terminate) {} // busy waiting since the other thread is on it
        }
        return msg;
    }
    void writeData(volatile char* rawData, size_t cap, const Message& msg) {
        size_t pos = 0;
        InPlaceData& data = *InPlaceData::getMetadata(rawData);
        auto [buf, buflen] = InPlaceData::getDataBuffer(rawData, cap);
        buflen -= (buflen % sizeof(int)); // must be a multiple of the data type
        int iteration = 1;

        while (data.available && !_terminate) usleep(1000);
        while (!_terminate) {
            data.tag = msg.tag;
            size_t end = std::min(pos + buflen, msg.userData.size()*sizeof(int));
            data.size = end-pos;
            memcpy((char*)buf, msg.userData.data() + pos / sizeof(int), data.size);
            bool tbc = (pos/sizeof(int) < msg.userData.size());
            data.toBeContinued = tbc;
            pos += data.size;
            data.available = true;

            if (!tbc) break; // done!
            while (data.available && !_terminate) {} // busy waiting since the other thread is on it
        }
    }

    BackgroundWorker _bg_reader;
    BackgroundWorker _bg_writer;
    SPSCBlockingRingbuffer<Message> _buf_in;
    SPSCBlockingRingbuffer<Message> _buf_out;

    Message _msg_to_read;

public:
    BiDirectionalAnytimePipeShmem(char* dataOut, size_t sizeOut, char* dataIn, size_t sizeIn, bool parent) :
        _data_out(dataOut), _cap_out(sizeOut), _data_in(dataIn), _cap_in(sizeIn),
        _buf_in(64), _buf_out(64) {

        if (parent) {
            InPlaceData::getMetadata(_data_in)->available = false;
            InPlaceData::getMetadata(_data_out)->available = false;
        }

        _bg_reader.run([&]() {
            Message msg;
            while (!_terminate) { // run indefinitely until you should terminate
                // read message from the pipe - blocks until parent writes something
                msg = readData(_data_in, _cap_in);
                if (msg.tag == 0) break;
                bool success = _buf_in.pushBlocking(msg);
                if (!success) break;
            }
        });
        _bg_writer.run([&]() {
            Message msg;
            while (!_terminate) { // run until terminating
                // read message from writing queue
                bool success = _buf_out.pollBlocking(msg);
                if (!success) break;
                writeData(_data_out, _cap_out, msg);
                _nb_written++;
            }
        });
    }

    char pollForData(bool abortAtFailure = true) {
        if (_buf_in.empty()) return 0;
        bool ok = _buf_in.pollBlocking(_msg_to_read);
        if (!ok) {
            if (abortAtFailure) abort();
            return 0;
        }
        LOG(V5_DEBG, "PIPE read %c\n", _msg_to_read.tag);
        return _msg_to_read.tag;
    }
    std::vector<int> readData(char& contentTag) {
        const char expectedTag = contentTag;
        contentTag = _msg_to_read.tag;
        assert(expectedTag == contentTag);
        return std::move(_msg_to_read.userData);
    }

    bool writeData(const std::vector<int>& data, char contentTag) {
        return writeData(std::vector<int>(data), contentTag);
    }
    bool writeData(const std::vector<int>& data1, const std::vector<int>& data2, char contentTag) {
        return writeData(std::vector<int>(data1), data2, contentTag);
    }

    bool writeData(std::vector<int>&& data, char contentTag) {
        LOG(V5_DEBG, "PIPE write %c\n", contentTag);
        Message msg {contentTag, std::move(data)};
        bool success = _buf_out.pushBlocking(msg);
        if (success) _nb_to_write++;
        return success;
    }
    bool writeData(std::vector<int>&& data1, const std::vector<int>& data2, char contentTag) {
        LOG(V5_DEBG, "PIPE write %c\n", contentTag);
        data1.insert(data1.end(), data2.begin(), data2.end());
        Message msg {contentTag, std::move(data1)};
        bool success = _buf_out.pushBlocking(msg);
        if (success) _nb_to_write++;
        return success;
    }

    void flush() {
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

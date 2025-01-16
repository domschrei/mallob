
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
        volatile char* data;
        bool& available() {return * (bool*) data;}
        size_t& size() {return * (size_t*) (data + 1);}
        bool& toBeContinued() {return * (bool*) (data + 1 + sizeof(size_t));}
        char& tag() {return * (char*) (data + 1 + sizeof(size_t) + 1);}
        char* payload() {return (char*) (data + 1 + sizeof(size_t) + 1 + 1);}
        size_t metadataSize() const {return 1 + sizeof(size_t) + 1 + 1;}
    };

    struct Message {
        char tag {0};
        std::vector<uint8_t> data;
        std::vector<int> toIntVec() const {
            return std::vector<int>((int*) data.data(), (int*) (data.data()+data.size()));
        }
        static std::vector<uint8_t> fromIntVec(const std::vector<int>& intvec) {
            return std::vector<uint8_t>((uint8_t*) intvec.data(), (uint8_t*) (intvec.data()+intvec.size()));
        }
    };

    Message readData(volatile char* rawData, size_t cap) {
        Message msg;
        InPlaceData data {rawData};
        int iteration = 1;

        while (!data.available() && !_terminate) usleep(1000);
        while (!_terminate) {
            msg.tag = data.tag();
            size_t oldMsgSize = msg.data.size();
            msg.data.resize(msg.data.size() + data.size());
            memcpy(msg.data.data() + oldMsgSize, data.payload(), data.size());
            bool tbc = data.toBeContinued();
            data.available() = false;

            if (!tbc) break; // done!
            while (!data.available() && !_terminate) {} // busy waiting since the other thread is on it
        }
        return msg;
    }
    void writeData(volatile char* rawData, size_t cap, const Message& msg) {
        size_t pos = 0;
        InPlaceData data {rawData};
        int iteration = 1;

        while (data.available() && !_terminate) usleep(1000);
        while (!_terminate) {
            data.tag() = msg.tag;
            size_t end = std::min(pos + cap - data.metadataSize(), msg.data.size());
            data.size() = end-pos;
            memcpy(data.payload(), msg.data.data() + pos, data.size());
            bool tbc = (pos < msg.data.size());
            data.toBeContinued() = tbc;
            pos += data.size();
            data.available() = true;

            if (!tbc) break; // done!
            while (data.available() && !_terminate) {} // busy waiting since the other thread is on it
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
            InPlaceData in {_data_in};
            in.available() = 0;
            InPlaceData out {_data_out};
            out.available() = 0;
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
        return _msg_to_read.toIntVec();
    }

    bool writeData(const std::vector<int>& data, char contentTag) {
        LOG(V5_DEBG, "PIPE write %c\n", contentTag);
        Message msg {contentTag, Message::fromIntVec(data)};
        bool success = _buf_out.pushBlocking(msg);
        if (success) _nb_to_write++;
        return success;
    }
    bool writeData(const std::vector<int>& data1, const std::vector<int>& data2, char contentTag) {
        LOG(V5_DEBG, "PIPE write %c\n", contentTag);
        std::vector<uint8_t> concat = Message::fromIntVec(data1);
        concat.insert(concat.end(), (uint8_t*) data2.data(), (uint8_t*) (data2.data()+data2.size()));
        assert(concat.size() == (data1.size()+data2.size()) * sizeof(int));
        Message msg {contentTag, std::move(concat)};
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

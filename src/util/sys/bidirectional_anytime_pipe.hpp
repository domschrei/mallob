
#pragma once

#include <bits/types/FILE.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <poll.h>

#include "app/sat/proof/trusted/trusted_utils.hpp"
#include "util/logger.hpp"
#include "util/assert.hpp"
#include "util/spsc_blocking_ringbuffer.hpp"
#include "util/sys/background_worker.hpp"
#include "util/sys/fileutils.hpp"

class BiDirectionalAnytimePipe {

public:
    enum InitializationMode {CREATE, ACCESS};

private:
    InitializationMode _mode;
    std::string _path_out;
    std::string _path_in;
    FILE* _pipe_out;
    FILE* _pipe_in;

    BackgroundWorker _bg_reader;
    BackgroundWorker _bg_writer;
    volatile bool* volatile _child_ready_to_write;
    struct Message {
        char tag;
        std::vector<int> data;
    };
    SPSCBlockingRingbuffer<Message> _buf_in;
    SPSCBlockingRingbuffer<Message> _buf_out;

    char _read_tag = 0;
    Message _read_msg;

    bool _failed {false};

public:
    BiDirectionalAnytimePipe(InitializationMode mode, const std::string& fifoOut, const std::string& fifoIn, bool* shmemReadFlag) :
        _mode(mode), _path_out(fifoOut), _path_in(fifoIn), _child_ready_to_write(shmemReadFlag),
        _buf_in(128), _buf_out(128) {

        if (_mode == CREATE) {
            int res;
            res = mkfifo(_path_out.c_str(), 0666);
            assert(res == 0);
            res = mkfifo(_path_in.c_str(), 0666);
            assert(res == 0);
            *_child_ready_to_write = false;
        }
    }

    void open() {
        if (_mode == CREATE) {
            _pipe_out = fopen(_path_out.c_str(), "w");
            assert(_pipe_out);
            _pipe_in = fopen(_path_in.c_str(), "r");
            assert(_pipe_in);
        } else {
            _pipe_in = fopen(_path_in.c_str(), "r");
            assert(_pipe_in);
            _pipe_out = fopen(_path_out.c_str(), "w");
            assert(_pipe_out);

            // the child process uses additional threads for reading and writing,
            // ensuring that the parent process never needs to block
            _bg_reader.run([&]() {
                Message msg;
                while (true) { // run indefinitely until the pipe is closed!
                    // read message from the pipe - blocks until parent writes something
                    doReadFromPipe(&msg.tag, 1, 1, false);
                    if (msg.tag == 0 || _failed) break;
                    //printf("READING %c FROM PIPE\n", msg.tag);
                    msg.data = readFromPipe(false);
                    if (_failed) break;
                    //printf("READ %c FROM PIPE\n", msg.tag);
                    // write message into reading queue
                    bool success = _buf_in.pushBlocking(msg);
                    if (!success) LOG(V1_WARN, "[WARN] Unsuccessful pipe write for tag %c\n", msg.tag);
                    //printf("READ FROM PIPE TO QUEUE\n");
                }
            });
            _bg_writer.run([&]() {
                Message msg;
                while (_bg_writer.continueRunning()) { // run until terminating
                    // read message from writing queue
                    bool success = _buf_out.pollBlocking(msg);
                    if (!success) break;
                    // wait until the previous message has been read by the parent
                    while (*_child_ready_to_write) {usleep(1);}
                    //printf("CAN WRITE FROM QUEUE TO PIPE\n");
                    // signal to the parent that new message is available
                    *_child_ready_to_write = true;
                    // write message to the pipe - may block until parent reads it
                    writeToPipe(msg.data, msg.tag, false);
                    if (_failed) break;
                    //printf("WROTE FROM QUEUE TO PIPE\n");
                }
            });
        }
    }

    char pollForData(bool abortAtFailure = true) {
        if (_read_tag != 0) return _read_tag;
        if (_mode == CREATE) {
            // parent process checks if there is in fact some data ready to be read
            if (*_child_ready_to_write) {
                doReadFromPipe(&_read_tag, 1, 1, abortAtFailure);
                *_child_ready_to_write = false;
            }
        } else {
            // child process works via dedicated reading queue and side thread
            bool success = _buf_in.pollBlocking(_read_msg, true);
            if (success) {
                assert(_read_msg.tag != 0);
                _read_tag = _read_msg.tag;
            }
        }
        return _read_tag;
    }

    std::vector<int> readData(char& contentTag) {
        const char expectedTag = contentTag;
        contentTag = pollForData();
        assert(expectedTag == contentTag);
        _read_tag = 0; // reset tag
        if (_mode == CREATE) {
            // parent process uses plain reading here - if no waiting is required,
            // pollForData() should be used first to guarantee that data is available to be read.
            std::vector<int> out = readFromPipe(true);
            LOG(V5_DEBG, "[PIPE] read %i ints \"%c\"\n", out.size(), contentTag);
            return out;
        } else {
            // child process already copied the entire message in pollForData().
            LOG(V5_DEBG, "[PIPE] read %i ints \"%c\"\n", _read_msg.data.size(), contentTag);
            return std::move(_read_msg.data);
        }
    }

    void writeData(const std::vector<int>& data, char contentTag) {
        LOG(V5_DEBG, "[PIPE] write %i ints \"%c\"\n", data.size(), contentTag);
        if (_mode == CREATE) {
            // Parent process writes data immediately. It never needs to wait because
            // the child process has a separate thread that always, only, reads.
            writeToPipe(data, contentTag, true);
        } else {
            // Child process: write message to output buffer
            Message msg {contentTag, data};
            bool success = _buf_out.pushBlocking(msg);
            assert(success);
        }
    }
    void writeData(const std::vector<int>& data1, const std::vector<int>& data2, char contentTag) {
        LOG(V5_DEBG, "[PIPE] write %i ints \"%c\"\n", data1.size()+data2.size(), contentTag);
        if (_mode == CREATE) {
            // Parent process writes data immediately. It never needs to wait because
            // the child process has a separate thread that always, only, reads.
            writeToPipe(data1, data2, contentTag, true);
        } else {
            // Child process: write message to output buffer
            std::vector<int> concat = data1;
            concat.insert(concat.end(), data2.begin(), data2.end());
            assert(concat.size() == data1.size()+data2.size());
            Message msg {contentTag, std::move(concat)};
            bool success = _buf_out.pushBlocking(msg);
            assert(success);
        }
    }

    ~BiDirectionalAnytimePipe() {
        if (_mode == CREATE) {
            // Parent: Send a signal to the child process that we close the pipes.
            writeToPipe({}, 0, false);
            // Finish reading whatever the child process has sent in the meantime.
            while (!_failed) {
                char tag = pollForData(false);
                if (_failed) break;
                (void) readFromPipe(false);
            }
        } else {
            // Child: stop taking data from the buffers.
            _buf_in.markExhausted();
            _buf_in.markTerminated();
            _buf_out.markExhausted();
            _buf_out.markTerminated();
            // Join with the background threads once they are done.
            _bg_reader.stop();
            _bg_writer.stop();
        }
        fclose(_pipe_out);
        fclose(_pipe_in);
        if (_mode == CREATE) {
            FileUtils::rm(_path_out);
            FileUtils::rm(_path_in);
        }
    }

private:
    std::vector<int> readFromPipe(bool abortAtFailure) {
        int size;
        doReadFromPipe(&size, sizeof(int), 1, abortAtFailure);
        //printf("-- read size %i\n", size);
        std::vector<int> out(size);
        doReadFromPipe(out.data(), sizeof(int), size, abortAtFailure);
        //printf("-- read %i ints\n", size);
        return out;
    }
    void writeToPipe(const std::vector<int>& data, char tag, bool abortAtFailure) {
        doWriteToPipe(&tag, 1, 1, abortAtFailure);
        //printf("-- wrote tag %c\n", tag);
        if (MALLOB_LIKELY(tag != 0)) {
            const int intsize = data.size();
            assert(intsize == data.size());
            doWriteToPipe(&intsize, sizeof(int), 1, abortAtFailure);
            //printf("-- wrote size %i\n", intsize);
            doWriteToPipe(data.data(), sizeof(int), intsize, abortAtFailure);
            //printf("-- wrote %i ints\n", intsize);
        }
        fflush(_pipe_out);
    }
    void writeToPipe(const std::vector<int>& data1, const std::vector<int>& data2, char tag, bool abortAtFailure) {
        doWriteToPipe(&tag, 1, 1, abortAtFailure);
        //printf("-- wrote tag %c\n", tag);
        const int intsize = data1.size() + data2.size();
        assert(intsize == data1.size() + data2.size());
        doWriteToPipe(&intsize, sizeof(int), 1, abortAtFailure);
        //printf("-- wrote size %i\n", intsize);
        doWriteToPipe(data1.data(), sizeof(int), data1.size(), abortAtFailure);
        doWriteToPipe(data2.data(), sizeof(int), data2.size(), abortAtFailure);
        //printf("-- wrote %i ints\n", intsize);
        fflush(_pipe_out);
    }

    void doWriteToPipe(const void* data, int size, int nbElems, bool abortAtFailure) {
        const int nbWritten = fwrite(data, size, nbElems, _pipe_out);
        if (nbWritten < nbElems) {
            _failed = true;
            if (abortAtFailure) abort();
        }
        //printf("  -- wrote %i elems of size %i\n", nbWritten, size);
    }
    void doReadFromPipe(void* data, int size, int nbElems, bool abortAtFailure) {
        const int nbRead = fread(data, size, nbElems, _pipe_in);
        if (nbRead < nbElems) {
            _failed = true;
            if (abortAtFailure) abort();
        }
        //printf("  -- read %i elems of size %i\n", nbRead, size);
    }
};

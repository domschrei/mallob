
#pragma once

#include <bits/types/FILE.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/poll.h>
#include <sys/stat.h>
#include <vector>
#include <poll.h>

#include "util/logger.hpp"
#include "util/assert.hpp"
#include "util/sys/fileutils.hpp"

class BiDirectionalPipe {

public:
    enum Mode {CREATE, ACCESS};

private:
    Mode _mode;
    std::string _path_out;
    std::string _path_in;
    FILE* _pipe_out;
    FILE* _pipe_in;

    char _read_tag = 0;

public:
    BiDirectionalPipe(Mode mode, const std::string& fifoOut, const std::string& fifoIn) :
        _mode(mode), _path_out(fifoOut), _path_in(fifoIn) {

        if (_mode == CREATE) {
            int res;
            res = mkfifo(_path_out.c_str(), 0666);
            assert(res == 0);
            res = mkfifo(_path_in.c_str(), 0666);
            assert(res == 0);
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
        }
        // non-blocking reading
        for (auto stream : {_pipe_in}) {
            int fd = fileno(stream);
            int retval = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            assert(retval == 0);
        }
    }

    char pollForData() {
        if (_read_tag == 0) {
            int nbRead = fread(&_read_tag, 1, 1, _pipe_in);
            assert((nbRead == 0) == (_read_tag == 0));
        }
        return _read_tag;
    }

    std::vector<int> readData(char& contentTag, bool assertRightTag = false) {
        char expectedTag = contentTag;
        if (_read_tag == 0) {
            while (fread(&contentTag, 1, 1, _pipe_in) == 0) {}
        } else {
            contentTag = _read_tag;
            _read_tag = 0;
        }
        assert(contentTag != 0);
        if (assertRightTag) assert(expectedTag == contentTag);
        int size;
        readNonblocking(&size, sizeof(int));
        std::vector<int> out(size);
        readNonblocking(out.data(), size*sizeof(int));
        LOG(V5_DEBG, "[PIPE] read %i ints \"%c\"\n", size, contentTag);
        return out;
    }

    void writeData(const std::vector<int>& data, char contentTag) {
        writeData(data.data(), data.size(), contentTag);
    }
    void writeData(const std::vector<int>& data1, const std::vector<int>& data2, char contentTag) {
        writeData(data1.data(), data1.size(), data2.data(), data2.size(), contentTag);
    }

    void writeData(const int* data, size_t size, char contentTag) {
        assert(size < INT32_MAX);
        int isize = size;
        writeNonblocking(&contentTag, 1);
        writeNonblocking(&isize, sizeof(int));
        writeNonblocking(data, sizeof(int)*size);
        fflush(_pipe_out);
        LOG(V5_DEBG, "[PIPE] wrote %i ints \"%c\"\n", size, contentTag);
    }

    void writeData(const int* data1, size_t size1, const int* data2, size_t size2, char contentTag) {
        assert(size1+size2 < INT32_MAX);
        int totalSize = size1+size2;
        writeNonblocking(&contentTag, 1);
        writeNonblocking(&totalSize, sizeof(int));
        writeNonblocking(data1, sizeof(int)*size1);
        writeNonblocking(data2, sizeof(int)*size2);
        fflush(_pipe_out);
        LOG(V5_DEBG, "[PIPE] wrote %i ints \"%c\"\n", totalSize, contentTag);
    }

    ~BiDirectionalPipe() {
        fclose(_pipe_out);
        fclose(_pipe_in);
        if (_mode == CREATE) {
            FileUtils::rm(_path_out);
            FileUtils::rm(_path_in);
        }
    }

private:
    void readNonblocking(void* data, size_t size) {
        size_t pos = 0;
        while (pos < size) {
            const auto posBefore = pos;
            pos += fread(((char*) data)+pos, 1, size-pos, _pipe_in);
            if (pos < size && pos != posBefore) {
                LOG(V5_DEBG, "[PIPE] partial read %i/%i\n", pos, size);
            }
        }
        assert(pos == size);
    }

    void writeNonblocking(const void* data, size_t size) {
        size_t pos = 0;
        while (pos < size) {
            const auto posBefore = pos;
            pos += fwrite(((const char*) data)+pos, 1, size-pos, _pipe_out);
            if (pos < size && pos != posBefore) {
                LOG(V5_DEBG, "[PIPE] partial write %i/%i\n", pos, size);
            }
        } 
        assert(pos == size);
    }

};

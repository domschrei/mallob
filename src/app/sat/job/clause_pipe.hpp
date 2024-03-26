
#pragma once

#include <bits/types/FILE.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

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
    }

    std::vector<int> readData() {
        int size, nbRead;
        nbRead = fread(&size, sizeof(int), 1, _pipe_in);
        assert(nbRead == 1);
        std::vector<int> out(size);
        nbRead = fread(out.data(), sizeof(int), size, _pipe_in);
        assert(nbRead == size);
        LOG(V5_DEBG, "[PIPE] read %i ints\n", size);
        return out;
    }

    void writeData(const std::vector<int>& data) {
        writeData(data.data(), data.size());
    }

    void writeData(const int* data, size_t size) {
        assert(size < INT32_MAX);
        int nbWritten;
        nbWritten = fwrite(&size, sizeof(int), 1, _pipe_out);
        assert(nbWritten == 1);
        nbWritten = fwrite(data, sizeof(int), size, _pipe_out);
        assert(nbWritten == size);
        fflush(_pipe_out);
        LOG(V5_DEBG, "[PIPE] wrote %i ints\n", size);
    }

    ~BiDirectionalPipe() {
        fclose(_pipe_out);
        fclose(_pipe_in);
        if (_mode == CREATE) {
            FileUtils::rm(_path_out);
            FileUtils::rm(_path_in);
        }
    }
};

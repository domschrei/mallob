
#pragma once

#include <fstream>

#define READ_BUFFER_SIZE 131072

class BufferedFileWriter {

    std::ofstream& stream;
    unsigned char write_buffer[READ_BUFFER_SIZE];
    size_t write_pos {0};

public:
    BufferedFileWriter(std::ofstream& stream) : stream(stream) {}
    ~BufferedFileWriter() {
        flush();
    }

    void put(unsigned char c) {
        if (write_pos == READ_BUFFER_SIZE) flush();
        write_buffer[write_pos++] = c;
    }
    void flush() {
        if (stream.good())
            stream.write((const char*) write_buffer, write_pos);
        write_pos = 0;
    }
};

class LinearFileReader {
public:
    virtual char next() = 0;
    virtual bool endOfFile() = 0;
};

class BufferedFileReader : public LinearFileReader {

    std::ifstream& ifs;
    char read_buffer[READ_BUFFER_SIZE];
    int read_pos {0};
    int max_pos {-1};
    bool eof {false};

public:
    BufferedFileReader(std::ifstream& ifs) : LinearFileReader(), ifs(ifs) {}

    virtual char next() override {
        if (read_pos > max_pos) {
            // refill
            ifs.read((char*) read_buffer, READ_BUFFER_SIZE);
            if (ifs.eof()) max_pos = ifs.gcount()-1;
            else max_pos = READ_BUFFER_SIZE-1;
            read_pos = 0;
            if (read_pos > max_pos) {
                eof = true;
                return 0;
            }
        }
        return read_buffer[read_pos++];
    }
    virtual bool endOfFile() override {
        return eof;
    }
};

#undef READ_BUFFER_SIZE

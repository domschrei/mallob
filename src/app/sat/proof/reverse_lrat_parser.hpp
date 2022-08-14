
#pragma once

#include <string>
#include <vector>
#include <stdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <sstream>

#include "util/assert.hpp"
#include "util/logger.hpp"
#include "lrat_line.hpp"

class ReverseLratParser {

private:
    int _fd;
    long _file_size;
    unsigned char* _mmap;
    long _pos;
    unsigned long _num_read_lines = 0;

    LratLine _line;

public:
    ReverseLratParser(const std::string& filename) {

        // Open file
        _fd = open(filename.c_str(), O_RDONLY);

        // Get file size
        struct stat s;
        int status = fstat(_fd, &s);
        _file_size = s.st_size;

        // Memory-map file, set position to last character
        _mmap = (unsigned char*) mmap(0, _file_size, PROT_READ, MAP_PRIVATE, _fd, 0);
        _pos = _file_size-1;

        // Parse first (last) line
        skipToFinalLineBreak();
        readNextLine();
    }

    ~ReverseLratParser() {
        munmap(_mmap, _file_size);
        close(_fd);
    }

    bool hasNext() const {
        return _line.valid();
    }

    LratLine next() {
        LratLine line = std::move(_line);
        assert(line.valid());
        
        // Parse next (previous) line
        _line.id = -1;
        _line.literals.clear();
        _line.hints.clear();
        _line.signsOfHints.clear();
        readNextLine();

        return line;
    }

    unsigned long getNumReadLines() const {
        return _num_read_lines;
    }

private:

    void skipToFinalLineBreak() {
        while (_pos > 0 && _mmap[_pos] != '\n') --_pos;
    }

    void readNextLine() {

        assert(!_line.valid());

        while (_pos >= 0) {
            if (tryReadLine()) return;
        }
    }

    bool tryReadLine() {

        // Skip over any line breaks
        while (_pos >= 0 && (_mmap[_pos] == '\n' || _mmap[_pos] == '\r')) {
            --_pos;
        }

        // Find the next line by seeking back to the previous line break
        auto lineSeekIdx = _pos;
        while (true) {
            if (lineSeekIdx == -1 || _mmap[lineSeekIdx] == '\n' || _mmap[lineSeekIdx] == '\r') {
                // End of the previous line (or beginning of the file)
                // This line begins *after* this index
                lineSeekIdx++;
                break;
            }
            lineSeekIdx--;
        }

        std::string lineStr(_mmap+lineSeekIdx, _mmap+_pos+1);
        LOG(V5_DEBG, "[LRAT] Found line: \"%s\"\n", lineStr.c_str());
        
        // Now our line sits at _mmap[lineSeekIdx] through _mmap[_pos] (inclusive).
        bool success;
        _line = LratLine((const char*) (_mmap+lineSeekIdx), _pos-lineSeekIdx+1, success);
        if (_line.id == -1)
            LOG(V5_DEBG, "[LRAT] Interpreted line: (skipped)\n");
        else {
            std::string lits;
            for (auto lit : _line.literals) lits += std::to_string(lit) + " ";
            std::string hints;
            for (auto hint : _line.hints) hints += std::to_string(hint) + " ";
            LOG(V5_DEBG, "[LRAT] Interpreted line: id=%ld lits=(%s) hints=(%s)\n", 
                _line.id, lits.c_str(), hints.c_str());
        }

        // Set new position to the index *before* the line just read
        _pos = lineSeekIdx-1;
        _num_read_lines++;

        return success;
    }
};

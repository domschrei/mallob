
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

#include "util/assert.hpp"
#include "util/logger.hpp"

class ReverseLratParser {

public:
    struct LratLine {
        unsigned long id = -1;
        std::vector<int> literals;
        std::vector<long> hints;
        bool valid() const {return id != -1;}
    };

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
        readNextLine();

        return line;
    }

    unsigned long getNumReadLines() const {
        return _num_read_lines;
    }

private:
    void readNextLine() {

        assert(!_line.valid());

        while (_pos >= 0) {
            if (tryReadLine()) return;
        }
    }

    bool tryReadLine() {

        // Advance _pos until no line breaks are read
        while (_pos >= 0 && (_mmap[_pos] == '\n' || _mmap[_pos] == '\r')) {
            --_pos;
        }

        // Find the next line
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
        
        bool beganNum = false;
        long num = 0;
        int sign = 1;

        bool readingId = true;
        bool readingClause = false;
        bool readingHints = false;
        bool success = false;

        // Function to publish a number which has been read completely
        auto publishReadNumber = [&]() {
            num *= sign;
            if (readingId) {
                _line.id = num;
                readingId = false;
                readingClause = true;
            } else if (readingClause) {
                if (num == 0) {
                    // Clause done
                    readingClause = false;
                    readingHints = true;
                } else {
                    // Add literal to clause
                    _line.literals.push_back(num);
                }
            } else if (readingHints) {
                if (num == 0) {
                    readingHints = false;
                    success = true;
                } else {
                    _line.hints.push_back(num);
                }
            }
            num = 0;
            sign = 1;
        };
        
        // Iterate over all characters of the found line
        // (in correct / forward / left-to-right order)
        for (auto i = lineSeekIdx; i <= _pos; ++i) {
            bool cancel = false;
            switch (_mmap[i]) {
                case '\n': case '\r': case ' ': case '\t':
                    if (beganNum) publishReadNumber();
                    break;
                case 'd':
                    cancel = true; // skip deletion lines
                    break;
                case '-':
                    sign *= -1;
                    beganNum = true;
                    break;
                default:
                    // Add digit to current number
                    num = num*10 + (_mmap[i]-'0');
                    beganNum = true;
            }
            if (cancel) {
                _line.id = -1;
                break;
            }
        }
        if (beganNum) publishReadNumber();

        if (_line.id == -1)
            LOG(V5_DEBG, "[LRAT] Interpreted line: (skipped)\n");
        else {
            std::string lits;
            for (int lit : _line.literals) lits += std::to_string(lit) + " ";
            std::string hints;
            for (int hint : _line.hints) hints += std::to_string(hint) + " ";
            LOG(V5_DEBG, "[LRAT] Interpreted line: id=%ld lits=(%s) hints=(%s)\n", 
                _line.id, lits.c_str(), hints.c_str());
        }

        // Set new position to the index *before* the line just read
        _pos = lineSeekIdx-1;
        _num_read_lines++;

        return success;
    }
};

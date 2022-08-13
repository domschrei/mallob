
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

typedef unsigned long LratClauseId;

class ReverseLratParser {

public:
    struct LratLine {
        LratClauseId id = -1;
        std::vector<int> literals;
        std::vector<LratClauseId> hints;
        std::vector<bool> signsOfHints;
        bool valid() const {return id != -1;}
        std::string toStr() const {
            std::stringstream out;
            out << id;
            for (auto lit : literals) out << " " << lit ;
            out << " 0";
            for (size_t i = 0; i < hints.size(); i++) {
                out << " " << (signsOfHints[i] ? "" : "-") << hints[i];
            }
            out << " 0\n";
            return out.str();
        }
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
        
        bool beganNum = false;
        unsigned long num = 0;
        int sign = 1;

        bool readingId = true;
        bool readingClause = false;
        bool readingHints = false;
        bool success = false;

        // Function to publish a number which has been read completely
        auto publishReadNumber = [&]() {
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
                    _line.literals.push_back(sign * num);
                }
            } else if (readingHints) {
                if (num == 0) {
                    readingHints = false;
                    success = true;
                } else {
                    _line.hints.push_back(num);
                    _line.signsOfHints.push_back(sign > 0);
                }
            }
            num = 0;
            sign = 1;
            beganNum = false;
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
                case '0':
                case '1':
                case '2':
                case '3':
                case '4':
                case '5':
                case '6':
                case '7':
                case '8':
                case '9':
                    // Add digit to current number
                    num = num*10 + (_mmap[i]-'0');
                    beganNum = true;
                    break;
                default:
                    LOG(V0_CRIT, "[ERROR] Unexpected character \"%c\" (code: %i) in LRAT file!\n", 
                        _mmap[i], _mmap[i]);
                    abort();
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

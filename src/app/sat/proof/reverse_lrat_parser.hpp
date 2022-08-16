
#pragma once

#include <string>
#include <vector>
#include <fstream>

#include "util/assert.hpp"
#include "util/logger.hpp"
#include "lrat_line.hpp"

#define LRAT_LINE_SIZE_LIMIT 65536

class ReverseLratParser {

private:
    FILE* _tac_process;
    char _buffer[LRAT_LINE_SIZE_LIMIT];
    bool _process_done = false;
    
    unsigned long _num_read_lines = 0;

    LratLine _line;

public:
    ReverseLratParser(const std::string& filename) {

        // Open file
        std::string cmd = "tac " + filename;
        _tac_process = popen(cmd.c_str(), "r");

        // Parse first (last) line
        readNextLine();
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

    std::string getNextLine() {

        if (feof(_tac_process)) {
            _process_done = true;
            return "";
        }
        
        // Try to refill buffer
        if (fgets(_buffer, LRAT_LINE_SIZE_LIMIT, _tac_process) == NULL) return "";

        // Read proper line from buffer
        std::string line;
        bool foundLinebreak = false;
        for (size_t i = 0; i < LRAT_LINE_SIZE_LIMIT; i++) {
            if (_buffer[i] == '\n') {
                foundLinebreak = true;
                break;
            }
            line += _buffer[i];
        }
        assert(foundLinebreak);

        //LOG(V5_DEBG, "[LRAT] Line candidate: \"%s\"\n", 
        //    line.c_str());
        return line;
    }

    void readNextLine() {

        assert(!_line.valid());

        while (!_process_done) {
            if (tryReadLine()) return;
        }
    }

    bool tryReadLine() {

        std::string line = getNextLine();
        if (line.empty()) return false;

        LOG(V5_DEBG, "[LRAT] Found line: \"%s\"\n", line.c_str());
        
        bool success;
        _line = LratLine(line.c_str(), line.size(), success);
        /*
        if (!success || _line.id == -1)
            LOG(V5_DEBG, "[LRAT] Interpreted line: (skipped)\n");
        else {
            std::string lits;
            for (auto lit : _line.literals) lits += std::to_string(lit) + " ";
            std::string hints;
            for (auto hint : _line.hints) hints += std::to_string(hint) + " ";
            LOG(V5_DEBG, "[LRAT] Interpreted line: id=%ld lits=(%s) hints=(%s)\n", 
                _line.id, lits.c_str(), hints.c_str());
        }
        */
        return success;
    }
};

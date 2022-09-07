
#pragma once

#include <fstream>

#include "lrat_line.hpp"
#include "serialized_lrat_line.hpp"

namespace lrat_utils {
    enum WriteMode {NORMAL, REVERSED};
    void writeLine(std::ofstream& ofs, const LratLine& line);
    void writeLine(std::ofstream& ofs, const SerializedLratLine& line, WriteMode mode = NORMAL);
    void writeDeletionLine(std::ofstream& ofs, LratClauseId headerId, const std::vector<unsigned long>& ids, WriteMode mode = NORMAL);
    bool readLine(std::ifstream& ifs, LratLine& line);
}

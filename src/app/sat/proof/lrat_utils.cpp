
#include "lrat_utils.hpp"
#include "util/assert.hpp"



void writeLineHeader(std::ofstream& ofs) {
    char header = 'a';
    ofs.write((const char*) &header, 1);
}

void writeDeletionLineHeader(std::ofstream& ofs) {
    char header = 'd';
    ofs.write((const char*) &header, 1);
}

void writeSeparator(std::ofstream& ofs) {
    char zero = '\0';
    ofs.write((const char*) &zero, 1);
}

void writeVariableLengthUnsigned(std::ofstream& ofs, int64_t n) {
    assert (n > 0);
    unsigned char ch;
    while (n & ~0x7f) {
        ch = (n & 0x7f) | 0x80;
        ofs.write((const char*) &ch, 1);
        n >>= 7;
    }
    ch = n;
    ofs.write((const char*) &ch, 1);
}

void writeVariableLengthUnsignedReversed(std::ofstream& ofs, int64_t n) {
    assert (n > 0);
    
    auto original = n;
    int numIterations = 0;
    while (n & ~0x7f) {
        numIterations++;
        n >>= 7;
    }

    unsigned char ch = n;
    ofs.write((const char*) &ch, 1);

    for (int i = 1; i <= numIterations; i++) {
        n = original >> (7*(numIterations - i));
        ch = (n & 0x7f) | 0x80;
        ofs.write((const char*) &ch, 1);
    }
}

void writeVariableLengthSigned(std::ofstream& ofs, int64_t n) {
    writeVariableLengthUnsigned(ofs, 2*std::abs(n) + (n < 0));
}

void writeVariableLengthSignedReversed(std::ofstream& ofs, int64_t n) {
    writeVariableLengthUnsignedReversed(ofs, 2*std::abs(n) + (n < 0));
}

void writeSignedClauseId(std::ofstream& ofs, int64_t id, lrat_utils::WriteMode mode) {
    if (mode == lrat_utils::REVERSED) {
        writeVariableLengthSignedReversed(ofs, id);
    } else {
        writeVariableLengthSigned(ofs, id);
    }
}

void writeLiteral(std::ofstream& ofs, int lit, lrat_utils::WriteMode mode) {
    if (mode == lrat_utils::REVERSED) {
        writeVariableLengthSignedReversed(ofs, lit);
    } else {
        writeVariableLengthSigned(ofs, lit);
    }
}



bool readSignedClauseId(std::ifstream& ifs, int64_t& id) {
    int64_t unadjusted = 0;
    int64_t coefficient = 1;
    int32_t tmp = ifs.get();
    if (tmp == 0) return false;
    while (tmp) {
        // continuation bit set?
        if (tmp & 0b10000000) {
            unadjusted += coefficient * (tmp & 0b01111111); // remove first bit
        } else {
            // last byte
            unadjusted += coefficient * tmp; // first bit is 0, so can leave it
            break;
        }
        coefficient *= 128; // 2^7 because we essentially have 7-bit bytes
        tmp = ifs.get(); //*((unsigned char*) (_num_buffer+i));
    }
    if (unadjusted % 2) { // odds map to negatives
        id = -(unadjusted - 1) / 2;
    } else {
        id = unadjusted / 2;
    }
    return true;
}

bool readLiteral(std::ifstream& ifs, int32_t& lit) {
    int32_t unadjusted = 0;
    int32_t coefficient = 1;
    int32_t tmp = ifs.get();
    if (tmp == 0) return false;
    while (tmp >= 0) {
        // continuation bit set?
        if (tmp & 0b10000000) {
            unadjusted += coefficient * (tmp & 0b01111111); // remove first bit
        } else {
            // last byte
            unadjusted += coefficient * tmp; // first bit is 0, so can leave it
            break;
        }
        coefficient *= 128; // 2^7 because we essentially have 7-bit bytes
        tmp = ifs.get();
    }
    if (unadjusted % 2) { // odds map to negatives
        lit = -(unadjusted - 1) / 2;
    } else {
        lit = unadjusted / 2;
    }
    return true;
}



namespace lrat_utils {

    void writeLine(std::ofstream& ofs, const LratLine& line) {
        writeLineHeader(ofs);
        int64_t signedId = line.id;
        writeSignedClauseId(ofs, signedId, WriteMode::NORMAL);
        for (int lit : line.literals) {
            writeLiteral(ofs, lit, WriteMode::NORMAL);
        }
        writeSeparator(ofs);
        for (size_t i = 0; i < line.hints.size(); i++) {
            int64_t signedId = (line.signsOfHints[i] ? 1 : -1) * line.hints[i];
            writeSignedClauseId(ofs, signedId, WriteMode::NORMAL);
        }
        writeSeparator(ofs);
    }

    void writeLine(std::ofstream& ofs, const SerializedLratLine& line, WriteMode mode) {
        if (mode == REVERSED) {
            writeSeparator(ofs);
            auto [hints, numHints] = line.getUnsignedHints();
            auto signs = line.getSignsOfHints();
            for (int i = numHints-1; i >= 0; i--) {
                int64_t signedId = (signs[i] ? 1 : -1) * hints[i];
                writeSignedClauseId(ofs, signedId, mode);
            }
            writeSeparator(ofs);
            auto [lits, size] = line.getLiterals();
            for (int i = size-1; i >= 0; i--) {
                writeLiteral(ofs, lits[i], mode);
            }
            int64_t signedId = line.getId();
            writeSignedClauseId(ofs, signedId, mode);
            writeLineHeader(ofs);
        } else {
            writeLineHeader(ofs);
            int64_t signedId = line.getId();
            writeSignedClauseId(ofs, signedId, mode);
            auto [lits, size] = line.getLiterals();
            for (size_t i = 0; i < size; i++) {
                writeLiteral(ofs, lits[i], mode);
            }
            writeSeparator(ofs);
            auto [hints, numHints] = line.getUnsignedHints();
            auto signs = line.getSignsOfHints();
            for (size_t i = 0; i < numHints; i++) {
                int64_t signedId = (signs[i] ? 1 : -1) * hints[i];
                writeSignedClauseId(ofs, signedId, mode);
            }
            writeSeparator(ofs);
        }
    }

    void writeDeletionLine(std::ofstream& ofs, LratClauseId headerId, 
            const std::vector<unsigned long>& ids, WriteMode mode) {
        if (mode == REVERSED) {
            writeSeparator(ofs);
            for (int i = ids.size()-1; i >= 0; i--) {
                writeSignedClauseId(ofs, ids[i], mode);
            }
            writeDeletionLineHeader(ofs);
        } else {
            writeDeletionLineHeader(ofs);
            for (auto id : ids) {
                writeSignedClauseId(ofs, id, mode);
            }
            writeSeparator(ofs);
        }
    }

    bool readLine(std::ifstream& ifs, LratLine& line) {
        
        if (!ifs.good() || ifs.eof()) return false;

        line.id = -1;
        line.literals.clear();
        line.hints.clear();
        line.signsOfHints.clear();

        int header = ifs.get();
        if (header != 'a') return false;

        int64_t signedId;
        if (!readSignedClauseId(ifs, signedId)) return false;
        assert(signedId > 0);
        line.id = signedId;

        int lit;
        while (readLiteral(ifs, lit)) {
            line.literals.push_back(lit);
        }
        // separator zero was read by "readLiteral" call that returned zero

        while (readSignedClauseId(ifs, signedId)) {
            line.hints.push_back(std::abs(signedId));
            line.signsOfHints.push_back(signedId>0);
        }
        // line termination zero was read by "readLiteral" call that returned zero

        return true;
    }
}

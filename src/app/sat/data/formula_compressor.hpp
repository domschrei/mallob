
#pragma once

#include "global.hpp"
#include "robin_map.h"
#include "util/assert.hpp"
#include "util/logger.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

class FormulaCompressor {

public:
    struct CompressionInput {
        tsl::robin_map<int, std::vector<int>> table;
        int maxClauseLength {0};
        std::vector<int> assumptions;
    };

    struct FormulaOutput {
        void* data;
        size_t capacity;
        size_t size {0};
        bool error {false};

        // output statistics
        size_t fSize {0}; // # bytes excluding assumptions block
        size_t aSize {0}; // # bytes for assumptions
        int maxVar {0};
        int nbClauses {0};
        int nbAssumptions {0};

        virtual bool resize(size_t requestedCapacity) = 0;

        inline bool push(void* pushData, size_t pushLen) {
            if (size+pushLen >= capacity
                && !resize(
                    std::max(capacity+pushLen, (size_t)(1.2*capacity+1))
                ) && !resize(capacity+pushLen)) {
                error = true;
                return false;
            }
            memcpy(((unsigned char*) data)+size, pushData, pushLen);
            size += pushLen;
            return true;
        }
        inline bool push(unsigned char c) {
            return push(&c, 1);
        }
    };
    template <typename T>
    struct VectorFormulaOutput : public FormulaOutput {
        std::vector<T>* vec;
        bool owned {true};
        size_t sizeBefore {0};
        VectorFormulaOutput() {
            vec = new std::vector<T>();
            capacity = 0;
        }
        VectorFormulaOutput(std::vector<T>* vec) {
            this->vec = vec;
            capacity = 0;
            owned = false;
            sizeBefore = vec->size();
        }
        ~VectorFormulaOutput() {
            if (owned) delete vec;
        }
        bool resize(size_t requestedCapacity) override {
            bool shrink = requestedCapacity < capacity;
            vec->resize(sizeBefore + (size_t) std::ceil(requestedCapacity / (double) sizeof(T)));
            if (shrink) vec->shrink_to_fit();
            data = vec->data() + sizeBefore;
            capacity = sizeof(T) * (vec->size() - sizeBefore);
            return true;
        }
        std::vector<T>&& extract() {
            assert(!error);
            return std::move(*vec);
        }
    };

    struct CompressedFormulaView {
        const unsigned char* data;
        size_t size;

        size_t pos {0};

        int maxClauseLength {0};
        int clauseLength {0};

        int nbClauses {0};
        int cpos {0};

        int lpos {0};
        int lastLit {0};

        int nbAssumptions {-1};
        int apos {0};

        bool hasLitToExport {false};
        bool exportingAsmpt {false};
        int litToExport;

        inline bool getNextLit(int& lit) {
            if (!hasLitToExport) {
                bool ok = getNext(litToExport);
                if (!ok) return false;
                hasLitToExport = true;
            }
            if (exportingAsmpt) return false;
            lit = litToExport;
            hasLitToExport = false;
            return true;
        }
        inline bool getNextAssumption(int& lit) {
            if (!hasLitToExport) {
                bool ok = getNext(litToExport);
                if (!ok) return false;
                hasLitToExport = true;
            }
            if (!exportingAsmpt) return false;
            lit = litToExport;
            hasLitToExport = false;
            return true;
        }

        inline bool getNext(int& res) {

            // header: max clause length
            if (pos == 0) {
                pos += readVariableLengthUnsigned(data+pos, maxClauseLength);
            }

            // assumptions
            if (cpos == nbClauses && clauseLength == maxClauseLength && pos < size) {
                if (nbAssumptions < 0) {
                    // read padding to next word
                    while (pos % 4 != 0) {
                        int zero;
                        pos += readFixedBytesInteger(data+pos, 1, zero);
                        assert(zero == 0);
                    }

                    pos += readVariableLengthUnsigned(data+pos, nbAssumptions);
                    LOG(V2_INFO, "reading %i assumptions\n", nbAssumptions);
                    lastLit = 0;
                }
                if (apos < nbAssumptions) {
                    int ilit;
                    pos += readVariableLengthUnsigned(data+pos, ilit);
                    ilit += lastLit;
                    lastLit = ilit;
                    int elit = decompressLiteral(ilit);
                    apos++;
                    res = elit;
                    exportingAsmpt = true;
                    return true;
                }
                // assumptions done - read padding to next word
                while (pos < size && pos % 4 != 0) {
                    int zero;
                    pos += readFixedBytesInteger(data+pos, 1, zero);
                    assert(zero == 0);
                }
            }

            // next block
            while (cpos == nbClauses && clauseLength < maxClauseLength) {
                unsigned char count = *(data+pos);
                if (count <= 4) {
                    pos++;
                    int nbEmptySlots = 1;
                    if (count > 0) pos += readFixedBytesInteger(data+pos, count, nbEmptySlots);
                    clauseLength += nbEmptySlots;
                    continue;
                }
                clauseLength++;
                pos += readVariableLengthUnsigned(data+pos, nbClauses);
                nbClauses -= 4;
                assert(nbClauses > 0);
                LOG(V2_INFO, "reading %i clauses of len %i\n", nbClauses, clauseLength);
                cpos = 0;
                lpos = 0;
                lastLit = 0;
            }

            // done
            if (pos == size) return false;

            // end clause of current block
            if (lpos == clauseLength && cpos < nbClauses) {
                lastLit = 0;
                lpos = 0;
                cpos++;
                res = 0;
                return true;
            }

            // next literal of current clause
            if (lpos < clauseLength) {
                int ilit;
                pos += readVariableLengthUnsigned(data+pos, ilit);
                ilit += lastLit;
                lastLit = ilit;

                int elit = decompressLiteral(ilit);
                lpos++;
                res = elit;
                return true;
            }

            return false;
        }
    };

public:
    static VectorFormulaOutput<int> compress(const int* data, size_t size, const int* aData, size_t aSize) {
        VectorFormulaOutput<int> out;
        size_t outBytes = compress(data, size, aData, aSize, out);
        return out;
    }
    static size_t compress(const int* data, size_t size, const int* aData, size_t aSize, FormulaOutput& out) {
        int maxClauseLength;
        auto in = normalizeInput(data, size, aData, aSize);
        return compressInput(in, out);   
    }
    static bool readAndCompress(const std::string& cnfPath, FormulaOutput& out) {
        std::ifstream file(cnfPath);
        if (!file.is_open()) return false;

        int nbVars;
        int nbClauses;
        CompressionInput in;

        std::string line;
        std::vector<int> cls;
        bool assumptions = false;
        while (getline(file, line)) {
            if (line.empty() || line[0] == 'c') {
                continue; // Skip comments and empty lines
            }
            std::istringstream iss(line);
            if (line[0] == 'p') {
                std::string p, cnf_str;
                iss >> p >> cnf_str >> nbVars >> nbClauses;
                continue;
            }
            if (line[0] == 'a') {
                std::string a; iss >> a;
                assumptions = true;
            }
            int literal;
            while (iss >> literal) {
                if (literal == 0) break; // End of clause
                if (assumptions) in.assumptions.push_back(literal);
                else cls.push_back(literal);
            }
            if (!assumptions) {
                int clauseLength = cls.size();
                auto& tableEntry = in.table[clauseLength];
                for (int lit : cls) {
                    out.maxVar = std::max(out.maxVar, std::abs(lit));
                    tableEntry.push_back(compressLiteral(lit));
                }
                std::sort(tableEntry.end() - clauseLength, tableEntry.end());
                in.maxClauseLength = std::max(in.maxClauseLength, clauseLength);
                out.nbClauses++;
                cls.clear();
            }
        }

        LOG(V2_INFO, "Compressing clause table ...\n");
        compressInput(in, out);
        return true;
    }

    static CompressedFormulaView getView(const unsigned char* data, size_t size) {
        return CompressedFormulaView {data, size};
    }

    static std::vector<int> decompressViaView(const unsigned char* data, size_t size) {
        auto view = getView(data, size);
        std::vector<int> dec;
        //auto trueDec = decompress(data, size);
        // Literals
        while (true) {
            int lit;
            if (!view.getNextLit(lit)) break;
            dec.push_back(lit);
            //LOG(V2_INFO, "LIT %i\n", lit);
            //assert(dec.back() == trueDec.at(dec.size()-1) || log_return_false("[ERROR] View-based decompressor wrong at position %i!\n", dec.size()-1));
        }
        dec.push_back(0); // separator zero indicating assumptions
        // Assumptions
        while (true) {
            int lit;
            if (!view.getNextAssumption(lit)) break;
            dec.push_back(lit);
        }
        dec.push_back(0);
        return dec;
    }

private:
    static CompressionInput normalizeInput(const int* data, size_t size, const int* aData, size_t aSize) {
        CompressionInput in;
        size_t clauseStart {0};
        in.maxClauseLength = 0;
        bool assumptions = false;
        for (size_t i = 0; i < size; i++) {
            if (data[i] != 0) continue;
            size_t clauseEnd = i;
            int clauseLength = clauseEnd - clauseStart;
            if (clauseLength == 0) {
                // separator for assumptions!
                assumptions = true;
            }
            //LOG(V2_INFO, "+ clause of length %i, %i bytes per lit\n", clauseLength, bytesPerLitInClause);
            if (assumptions) {
                for (int i = 0; i < clauseLength; i++) {
                    in.assumptions.push_back(compressLiteral(*(data+clauseStart+i)));
                }
            } else {
                auto& lits = in.table[clauseLength];
                for (int i = 0; i < clauseLength; i++) {
                    lits.push_back(compressLiteral(*(data+clauseStart+i)));
                }
                std::sort(lits.end() - clauseLength, lits.end());
                in.maxClauseLength = std::max(in.maxClauseLength, clauseLength);
            }
            clauseStart = i+1;
        }
        if (aData) {
            for (int i = 0; i < aSize; i++) in.assumptions.push_back(compressLiteral(aData[i]));
        }
        std::sort(in.assumptions.begin(), in.assumptions.end());
        return in;
    }

    static bool compressInput(CompressionInput& in, FormulaOutput& out) {

        // Header: max clause length
        if (!writeVariableLengthUnsigned(in.maxClauseLength, out)) return false;

        // Compress clauses
        int nbEmptySlots = 0;
        for (size_t len = 1; len <= in.maxClauseLength; len++) {
            auto& entry = in.table[len];
            if (entry.empty()) {
                nbEmptySlots++;
                continue;
            }
            if (nbEmptySlots > 0) {
                if (nbEmptySlots == 1) {
                    if (!out.push(0)) return false;
                } else {
                    if (!writeFixedBytesInteger(nbNeededBytes(nbEmptySlots), 1, out)) return false;
                    if (!writeFixedBytesInteger(nbEmptySlots, nbNeededBytes(nbEmptySlots), out)) return false;
                }
                nbEmptySlots = 0;
            }
            int nbClauses = entry.size() / len;
            LOG(V2_INFO, "writing %i clauses of len %i\n", nbClauses, len);
            if (!writeVariableLengthUnsigned(4+nbClauses, out)) return false;
            int eIdx = 0;
            for (int cIdx = 0; cIdx < nbClauses; cIdx++) {
                int lastLit = 0;
                for (int lIdx = 0; lIdx < len; lIdx++) {
                    int lit = entry[eIdx++];
                    assert(lit-lastLit >= 0);
                    if (!writeVariableLengthUnsigned(lit-lastLit, out)) return false;
                    lastLit = lit;
                }
            }
        }

        // Add padding to next 32-bit integer
        while (out.size % 4 != 0) {
            if (!writeFixedBytesInteger(0, 1, out)) return false;
        }
        out.fSize = out.size;

        // Compress assumptions
        LOG(V2_INFO, "writing %i assumptions\n", in.assumptions.size());
        if (!writeVariableLengthUnsigned(in.assumptions.size(), out)) return false;
        int lastAsmpt = 0;
        for (int asmpt : in.assumptions) {
            if (!writeVariableLengthUnsigned(asmpt-lastAsmpt, out)) return false;
            lastAsmpt = asmpt;
        }
        out.nbAssumptions = in.assumptions.size();

        // Add padding to next 32-bit integer
        while (out.size % 4 != 0) {
            if (!writeFixedBytesInteger(0, 1, out)) return false;
        }
        out.aSize = out.size - out.fSize;

        // shrink-to-fit output vector
        if (!out.resize(out.size)) return false;

        LOG(V2_INFO, "Compressed formula to %lu bytes\n", out.size);
        return true;
    }

    static int compressLiteral(int elit) {
        // -1 -> 0
        // 1  -> 1
        // -2 -> 2
        // 2  -> 3
        // -3 -> 4
        // ...
        return 2 * std::abs(elit) - 1 - (elit < 0);
    }
    static int decompressLiteral(int ilit) {
        return (1 + ilit / 2) * (ilit % 2 == 1 ? 1 : -1);
    }

    static unsigned char nbNeededBytes(int x) {
        return 1 + (x >= 128) + (x >= 32768) + (x >= 8388608);
    }

    static bool writeFixedBytesInteger(int x, unsigned char nbBytes, FormulaOutput& out) {
        //LOG(V2_INFO, "WRITE %i (%i bytes)\n", x, nbBytes);
        if (nbBytes >= 4 && !out.push((x >> 24) & 0b11111111)) return false;
        if (nbBytes >= 3 && !out.push((x >> 16) & 0b11111111)) return false;
        if (nbBytes >= 2 && !out.push((x >> 8) & 0b11111111)) return false;
        if (nbBytes >= 1 && !out.push((x >> 0) & 0b11111111)) return false;
        return true;
    }
    static size_t readFixedBytesInteger(const unsigned char* inData, unsigned char nbBytes, int& outData) {
        outData = 0;
        int inIdx = 0;
        if (nbBytes >= 4) outData += 16777216 * inData[inIdx++];
        if (nbBytes >= 3) outData += 65536 * inData[inIdx++];
        if (nbBytes >= 2) outData += 256 * inData[inIdx++];
        if (nbBytes >= 1) outData += 1 * inData[inIdx++];
        //LOG(V2_INFO, "READ %i (%i bytes)\n", outData, nbBytes);
        return nbBytes;
    }


    static bool writeVariableLengthUnsigned(int n, FormulaOutput& out) {
        if (n == 0) return out.push(0);
        unsigned char ch;
        while (n & ~0x7f) {
            ch = (n & 0x7f) | 0x80;
            if (!out.push(ch)) return false;
            n >>= 7;
        }
        ch = n;
        return out.push(ch);
    }
    static int readVariableLengthUnsigned(const unsigned char* inData, int& n) {
        int offset = 0;
        n = 0;
        int32_t coefficient = 1;
        int32_t tmp = inData[offset++];
        while (tmp != 0) {
            // continuation bit set?
            if (tmp & 0b10000000) {
                n += coefficient * (tmp & 0b01111111); // remove first bit
            } else {
                // last byte
                n += coefficient * tmp; // first bit is 0, so can leave it
                break;
            }
            coefficient *= 128; // 2^7 because we essentially have 7-bit bytes
            tmp = inData[offset++];
        }
        return offset;
    }
};

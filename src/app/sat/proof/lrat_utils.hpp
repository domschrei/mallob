
#pragma once

#include <fstream>

#include "lrat_line.hpp"
#include "serialized_lrat_line.hpp"

#define LRAT_READ_BUFFER_SIZE 131072

namespace lrat_utils {

    enum WriteMode {NORMAL, REVERSED};
    
    struct WriteBuffer {
    
        std::ofstream& stream;
        unsigned char write_buffer[131072];
        size_t write_pos {0};

        WriteBuffer(std::ofstream& stream) : stream(stream) {}

        void put(unsigned char c) {
            write_buffer[write_pos++] = c;
        }
        void flush() {
            stream.write((const char*) write_buffer, write_pos);
            write_pos = 0;
        }

        void writeLineHeader() {
            put('a');
            //char header = 'a';
            //ofs.write((const char*) &header, 1);
        }

        void writeDeletionLineHeader() {
            put('d');
            //char header = 'd';
            //ofs.write((const char*) &header, 1);
        }

        void writeSeparator() {
            put('\0');
            //char zero = '\0';
            //ofs.write((const char*) &zero, 1);
        }

        void writeVariableLengthUnsigned(int64_t n) {
            assert (n > 0);
            unsigned char ch;
            while (n & ~0x7f) {
                ch = (n & 0x7f) | 0x80;
                put(ch);
                //ofs.write((const char*) &ch, 1);
                n >>= 7;
            }
            ch = n;
            put(ch);
            //ofs.write((const char*) &ch, 1);
        }

        void writeVariableLengthUnsignedReversed(int64_t n) {
            assert (n > 0);
            
            auto original = n;
            int numIterations = 0;
            while (n & ~0x7f) {
                numIterations++;
                n >>= 7;
            }

            unsigned char ch = n;
            put(ch);
            //ofs.write((const char*) &ch, 1);

            for (int i = 1; i <= numIterations; i++) {
                n = original >> (7*(numIterations - i));
                ch = (n & 0x7f) | 0x80;
                put(ch);
                //ofs.write((const char*) &ch, 1);
            }
        }

        void writeVariableLengthSigned(int64_t n) {
            writeVariableLengthUnsigned(2*std::abs(n) + (n < 0));
        }

        void writeVariableLengthSignedReversed(int64_t n) {
            writeVariableLengthUnsignedReversed(2*std::abs(n) + (n < 0));
        }

        void writeSignedClauseId(int64_t id, lrat_utils::WriteMode mode) {
            if (mode == lrat_utils::REVERSED) {
                writeVariableLengthSignedReversed(id);
            } else {
                writeVariableLengthSigned(id);
            }
        }

        void writeLiteral(int lit, lrat_utils::WriteMode mode) {
            if (mode == lrat_utils::REVERSED) {
                writeVariableLengthSignedReversed(lit);
            } else {
                writeVariableLengthSigned(lit);
            }
        }
    };

    struct ReadBuffer {

        std::ifstream& ifs;
        unsigned char read_buffer[LRAT_READ_BUFFER_SIZE];
        int read_pos {0};
        int max_pos {-1};
        bool eof {false};

        ReadBuffer(std::ifstream& ifs) : ifs(ifs) {}

        unsigned char get() {
            if (read_pos > max_pos) {
                // refill
                ifs.read((char*) read_buffer, LRAT_READ_BUFFER_SIZE);
                if (ifs.eof()) max_pos = ifs.gcount()-1;
                else max_pos = LRAT_READ_BUFFER_SIZE-1;
                read_pos = 0;
                if (read_pos > max_pos) {
                    eof = true;
                    return 0;
                }
            }
            return read_buffer[read_pos++];
        }

        bool readSignedClauseId(int64_t& id) {
            int64_t unadjusted = 0;
            int64_t coefficient = 1;
            int32_t tmp = get();
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
                tmp = get(); //*((unsigned char*) (_num_buffer+i));
            }
            if (unadjusted % 2) { // odds map to negatives
                id = -(unadjusted - 1) / 2;
            } else {
                id = unadjusted / 2;
            }
            return true;
        }

        bool readLiteral(int32_t& lit) {
            int32_t unadjusted = 0;
            int32_t coefficient = 1;
            int32_t tmp = get();
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
                tmp = get();
            }
            if (unadjusted % 2) { // odds map to negatives
                lit = -(unadjusted - 1) / 2;
            } else {
                lit = unadjusted / 2;
            }
            return true;
        }
    };

    void writeLine(WriteBuffer& ofs, const LratLine& line);
    void writeLine(WriteBuffer& ofs, const SerializedLratLine& line, WriteMode mode = NORMAL);
    void writeDeletionLine(WriteBuffer& ofs, LratClauseId headerId, const std::vector<unsigned long>& ids, WriteMode mode = NORMAL);
    
    bool readLine(ReadBuffer& ifs, LratLine& line);
    bool readLine(ReadBuffer& ifs, SerializedLratLine& line);
}

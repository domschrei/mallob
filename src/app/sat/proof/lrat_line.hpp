
#pragma once

#include <vector>
#include <sstream>

#include "util/assert.hpp"
#include "util/logger.hpp"

typedef unsigned long LratClauseId;

struct LratLine {
    LratClauseId id = -1;
    std::vector<int> literals;
    std::vector<LratClauseId> hints;
    LratLine() {}
    LratLine(const char* string, long strlen, bool& success) {

        bool beganNum = false;
        unsigned long num = 0;
        int sign = 1;

        bool readingId = true;
        bool readingClause = false;
        bool readingHints = false;
        success = false;

        // Function to publish a number which has been read completely
        auto publishReadNumber = [&]() {
            if (readingId) {
                id = num;
                readingId = false;
                readingClause = true;
            } else if (readingClause) {
                if (num == 0) {
                    // Clause done
                    readingClause = false;
                    readingHints = true;
                } else {
                    // Add literal to clause
                    literals.push_back(sign * num);
                }
            } else if (readingHints) {
                if (num == 0) {
                    readingHints = false;
                    success = true;
                } else {
                    //assert(num < 1000000000000000000UL);
                    hints.push_back(num);
                }
            }
            num = 0;
            sign = 1;
            beganNum = false;
        };
        
        // Iterate over all characters of the found line
        // (in correct / forward / left-to-right order)
        for (size_t i = 0; i < strlen; ++i) {
            bool cancel = false;
            switch (string[i]) {
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
                    num = num*10 + (string[i]-'0');
                    beganNum = true;
                    break;
                default:
                    LOG(V0_CRIT, "[WARN] Unexpected character \"%c\" (code: %i) in LRAT file!\n", 
                        string[i], string[i]);
                    cancel = true;
                    break;
            }
            if (cancel) {
                id = -1;
                break;
            }
        }
        if (beganNum) publishReadNumber();
    }
    bool valid() const {return id != -1;}
    bool empty() const {return literals.empty() && hints.empty();}
    std::string toStr() const {
        std::stringstream out;
        if (isDeletionStatement()) {
            out << "d";
        } else {
            out << id;
            for (auto lit : literals) out << " " << lit ;
            out << " 0";
        }
        for (size_t i = 0; i < hints.size(); i++) {
            out << " " << hints[i];
        }
        out << " 0\n";
        return out.str();
    }
    size_t size() const {
        return sizeof(LratClauseId) 
            + sizeof(int) + literals.size()*sizeof(int) 
            + sizeof(int) + hints.size()*sizeof(LratClauseId);
    }

    LratClauseId& getId() {return id;}
    std::pair<const int*, int> getLiterals() const {
        return std::pair<const int*, int>(
            literals.data(), 
            literals.size()
        );
    }
    std::pair<const LratClauseId*, int> getHints() const {
        return std::pair<const LratClauseId*, int>(hints.data(), hints.size());
    }
    std::pair<LratClauseId*, int> getHints() {
        return std::pair<LratClauseId*, int>(hints.data(), hints.size());
    }

    bool isDeletionStatement() const {
        return id == 0;
    }
};

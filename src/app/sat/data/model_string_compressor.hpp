
#pragma once

#include <climits>
#include <cstdlib>
#include <fstream>
#include <vector>
#include <string>

#include "util/assert.hpp"
#include "util/logger.hpp"
#include "util/string_utils.hpp"

class ModelStringCompressor {

public:
    static std::string compress(const std::vector<int>& solution) {

        std::string out = std::to_string(solution.size() - 1) + ":";
        size_t pos = 1;
        while (pos < solution.size()) {
            int x = 0;
            for (int i = 0; i < 4; i++) {
                if (pos >= solution.size()) break;
                assert(solution[pos] == pos || solution[pos] == -pos);
                x += (1 << i) * (solution[pos] > 0);
                pos++;
            }
            assert(x >= 0 && x < 16);
            switch (x) {
            case 0: out += "0"; break;
            case 1: out += "1"; break;
            case 2: out += "2"; break;
            case 3: out += "3"; break;
            case 4: out += "4"; break;
            case 5: out += "5"; break;
            case 6: out += "6"; break;
            case 7: out += "7"; break;
            case 8: out += "8"; break;
            case 9: out += "9"; break;
            case 10: out += "a"; break;
            case 11: out += "b"; break;
            case 12: out += "c"; break;
            case 13: out += "d"; break;
            case 14: out += "e"; break;
            case 15: out += "f"; break;
            }
        }
        //LOG(V2_INFO, "MAXSAT COMPRESS %s ==> %s\n", StringUtils::getSummary(solution, INT_MAX).c_str(), out.c_str());
        return out;
    }

    static std::vector<int> decompress(const std::string& packed) {

        char* solutionStr;
        size_t nbVars = std::strtoul(packed.c_str(), &solutionStr, 10); // reads until ":"
        assert(solutionStr[0] == ':');
        std::vector<int> solution(nbVars+1, 0); // index 0 has a filler 0

        int strpos = 1; // after ":"
        int var = 1;
        while (solutionStr[strpos] != '\0') {
            char c = solutionStr[strpos];
            std::string cAsString(1, c);
            char* endptr;
            int num = std::strtol(cAsString.c_str(), &endptr, 16);
            assert(endptr - cAsString.c_str() == 1); // read exactly one character!
            if (var <= nbVars) solution[var] = (num & 1) ? var : -var;
            var++;
            if (var <= nbVars) solution[var] = (num & 2) ? var : -var;
            var++;
            if (var <= nbVars) solution[var] = (num & 4) ? var : -var;
            var++;
            if (var <= nbVars) solution[var] = (num & 8) ? var : -var;
            var++;
            strpos++;
        }
        //LOG(V2_INFO, "MAXSAT DECOMPRESS %s ==> %s\n", packed.c_str(), StringUtils::getSummary(solution, INT_MAX).c_str());

        return solution;
    }
};

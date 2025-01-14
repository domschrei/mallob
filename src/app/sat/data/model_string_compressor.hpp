
#pragma once

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

class ModelStringCompressor {

public:
    static std::string compress(const std::vector<int>& solution) {
        std::string out = std::to_string(solution.size() - 1) + ":";
        size_t pos = 1;
        while (pos < solution.size()) {
            int x = 1 * (solution[pos] > 0);
            if (pos+1 < solution.size())
                x += 2 * (solution[pos+1] > 0);
            if (pos+2 < solution.size())
                x += 4 * (solution[pos+2] > 0);
            if (pos+3 < solution.size())
                x += 8 * (solution[pos+3] > 0);
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
            pos += 4;
        }
        return out;
    }

    static std::vector<int> decompress(const std::string& packed) {
        char* solutionStr;
        size_t nbVars = std::strtoul(packed.c_str(), &solutionStr, 10); // reads until ":"
        std::vector<int> solution(nbVars+1, 0); // index 0 has a filler 0

        int strpos = 1; // after ":"
        int var = 1;
        while (solutionStr[strpos] != '\0') {
            char c = solutionStr[strpos];
            int num = std::strtol(&c, nullptr, 16);
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

        return solution;
    }
};


#pragma once

#include <vector>
#include <math.h>
#include "util/assert.hpp"

class VariableTranslator {

private:
    bool _empty = true;
    std::vector<int> _extra_variables;

public:
    void addExtraVariable(int latestOrigMaxVar) {
        assert(latestOrigMaxVar >= 0);
        int tldMaxVar = getTldLit(latestOrigMaxVar);
        do tldMaxVar++; while (!_extra_variables.empty() && _extra_variables.back() >= tldMaxVar);
        _extra_variables.push_back(tldMaxVar);
        _empty = false;
    }

    int getTldLit(int origLit) {
        if (_empty) return origLit;
        int absLit = std::abs(origLit);
        for (int tldExtraVar : _extra_variables) {
            if (tldExtraVar > absLit) break;
            absLit++;
        }
        return (origLit>0?1:-1) * absLit;
    }

    int getOrigLitOrZero(int tldLit) {
        if (_empty) return tldLit;
        int absLit = std::abs(tldLit);
        int shift = 0;
        for (int tldExtraVar : _extra_variables) {
            if (tldExtraVar >= absLit) {
                if (tldExtraVar == absLit) return 0; // is an extra var!
                break;
            }
            shift++;
        }
        return (tldLit>0?1:-1) * (absLit-shift);
    }

    const std::vector<int>& getExtraVariables() const {
        return _extra_variables;
    }
};

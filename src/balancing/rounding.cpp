
#include "balancing/rounding.h"

#include <math.h>

namespace Rounding {

std::map<int, int> getRoundedAssignments(int remainderIdx, int& sum, 
    const SortedDoubleSequence& remainders, const std::map<int, double>& assignments) {

    double remainder = remainderIdx < remainders.size() ? remainders[remainderIdx] : 1.0;
    //int occurrences =  remainderIdx < _remainders.size() ? _remainders.getOccurrences(remainderIdx) : 0;

    std::map<int, int> roundedAssignments;
    for (auto it : assignments) {

        double r = it.second - (int)it.second;
        
        if (r < remainder) 
            roundedAssignments[it.first] = std::floor(it.second);
        else 
            roundedAssignments[it.first] = std::ceil(it.second);
        
        sum += roundedAssignments[it.first];
    }
    return roundedAssignments;
}

float penalty(float utilization, float loadFactor) {
    float l = loadFactor;
    float u = utilization;

    float lowPenalty = -1/l * u + 1;
    float highPenalty = 1/(1-l) * u - l/(1-l);
    return std::max(lowPenalty, highPenalty);
}

};
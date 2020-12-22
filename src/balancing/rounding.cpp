
#include "balancing/rounding.hpp"

#include <cmath>

namespace Rounding {

robin_hood::unordered_map<int, int> getRoundedAssignments(int remainderIdx, int& sum, 
    const SortedDoubleSequence& remainders, const robin_hood::unordered_map<int, double>& assignments) {

    double remainder = remainderIdx < remainders.size() ? remainders[remainderIdx] : 1.0;
    //int occurrences =  remainderIdx < _remainders.size() ? _remainders.getOccurrences(remainderIdx) : 0;

    robin_hood::unordered_map<int, int> roundedAssignments;
    for (auto& [jobId, assignment] : assignments) {
        
        int floor = (int)assignment;
        double r = assignment - floor;
        int rounded = r < remainder ? floor : floor+1;
        roundedAssignments[jobId] = rounded;
        sum += rounded;
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

}
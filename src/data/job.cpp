
#include "job.h"

std::vector<int> Job::serialize() const {
    std::vector<int> packed;

    // Basic data
    packed.push_back(id);
    packed.push_back(rootRank);
    packed.push_back((int) (1000 * priority));

    // Clauses
    packed.insert(packed.begin() + 3, formula.begin(), formula.end());

    // Separator (additional zero)
    packed.push_back(0);

    // Assumptions
    for (unsigned int i = 0; i < assumptions.size(); i++) {
        packed.push_back(assumptions[i]);
    }

    // Closing zero
    packed.push_back(0);

    return packed;
}

void Job::deserialize(const std::vector<int>& packed) {

    int i = 0;
    id = packed[i++];
    rootRank = packed[i++];
    priority = 0.001f * packed[i++];

    // Clauses
    for (unsigned int pos = i; pos+1 < packed.size(); pos++) {
        if (packed[pos] == 0 && packed[pos+1] == 0) {
            formula.insert(formula.begin(), packed.begin()+i, packed.begin()+(pos+1));
            i = pos+1;
            break;
        }
        if (packed[pos+1] != 0) pos++;
    }
    // Assumptions
    for (unsigned int pos = i; pos < packed.size(); pos++) {
        if (packed[pos] == 0)
            break;
        assumptions.push_back(packed[pos]);
    }
}


#include "job_description.h"

std::vector<int> JobDescription::serialize() const {
    std::vector<int> packed;

    // Basic data
    packed.push_back(id);
    packed.push_back(rootRank);
    packed.push_back((int) (1000 * priority));

    // Clauses
    packed.insert(packed.begin() + 3, payload.begin(), payload.end());

    // Closing zero
    packed.push_back(0);

    return packed;
}

void JobDescription::deserialize(const std::vector<int>& packed) {

    int i = 0;
    id = packed[i++];
    rootRank = packed[i++];
    priority = 0.001f * packed[i++];

    // Clauses
    for (unsigned int pos = i; pos+1 < packed.size(); pos++) {
        if (packed[pos] == 0 && packed[pos+1] == 0) {
            payload.insert(payload.begin(), packed.begin()+i, packed.begin()+(pos+1));
            i = pos+1;
            break;
        }
        if (packed[pos+1] != 0) pos++;
    }
}

std::vector<int> JobResult::serialize() const {
    std::vector<int> packed;
    packed.push_back(id);
    packed.push_back(result);
    packed.insert(packed.begin()+2, solution.begin(), solution.end());
    return packed;
}

void JobResult::deserialize(const std::vector<int>& packed) {
    int i = 0;
    id = packed[i++];
    result = packed[i++];
    solution.insert(solution.begin(), packed.begin()+i, packed.end());
}
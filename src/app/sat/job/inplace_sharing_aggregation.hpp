
#pragma once

#include <vector>

struct InplaceClauseAggregation {

    std::vector<int>& buffer;
    InplaceClauseAggregation(std::vector<int>& buffer) : buffer(buffer) {}

    int& maxRevision() {return buffer[buffer.size()-4];}
    int& numInputLiterals() {return buffer[buffer.size()-3];}
    int& numAggregatedNodes() {return buffer[buffer.size()-2];}
    int& successfulSolver() {return buffer[buffer.size()-1];}

    void stripToRawBuffer() {
        buffer.pop_back();
        buffer.pop_back();
        buffer.pop_back();
        buffer.pop_back();
    }

    static int numMetadataInts() {return 3;}
    static InplaceClauseAggregation prepareRawBuffer(std::vector<int>& buffer,
            int maxRevision=-1, int numInputLits=0, int numAggregated=1, int winningSolverId=-1) {
        buffer.push_back(maxRevision);
        buffer.push_back(numInputLits);
        buffer.push_back(numAggregated);
        buffer.push_back(winningSolverId);
        return InplaceClauseAggregation(buffer);
    }
    static std::vector<int> neutralElem() {
        std::vector<int> out;
        prepareRawBuffer(out);
        return out;
    }
};

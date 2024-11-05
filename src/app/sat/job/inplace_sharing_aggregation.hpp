
#pragma once

#include "util/logger.hpp"
#include <cassert>
#include <cstddef>
#include <vector>
#include <climits>

struct InplaceClauseAggregation {

    std::vector<int>& buffer;
    InplaceClauseAggregation(std::vector<int>& buffer) : buffer(buffer) {}

    long long& bestFoundSolutionCost() {
        return * (long long*) (buffer.data() + (buffer.size()-4-sizeof(long long)/sizeof(int)));
    };
    int& maxRevision() {return buffer[buffer.size()-4];}
    int& numInputLiterals() {return buffer[buffer.size()-3];}
    int& numAggregatedNodes() {return buffer[buffer.size()-2];}
    int& successfulSolver() {return buffer[buffer.size()-1];}

    void stripToRawBuffer() {
        buffer.pop_back();
        buffer.pop_back();
        buffer.pop_back();
        buffer.pop_back();
        for (int i = 0; i < sizeof(long long)/sizeof(int); i++) buffer.pop_back();
    }

    void replaceClauses(const std::vector<int>& clauses) {
        assert(clauses.size() == buffer.size() - numMetadataInts() || log_return_false("[ERROR] %lu vs. %lu\n", clauses.size(), buffer.size()));
        for (size_t i = 0; i < clauses.size(); i++) {
            buffer[i] = clauses[i];
        }
    }

    static int numMetadataInts() {return 4 + sizeof(long long)/sizeof(int);}
    static InplaceClauseAggregation prepareRawBuffer(std::vector<int>& buffer,
            int maxRevision=-1, int numInputLits=0, int numAggregated=1, int winningSolverId=-1,
            long long bestFoundObjectiveCost=LLONG_MAX) {
        for (int i = 0; i < sizeof(long long)/sizeof(int); i++)
            buffer.push_back(* (((int*) &bestFoundObjectiveCost) + i));
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

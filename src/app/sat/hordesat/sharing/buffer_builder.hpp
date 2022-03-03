
#pragma once

#include <vector>

#include "app/sat/hordesat/sharing/database/buffer_iterator.hpp"
#include "app/sat/hordesat/utilities/clause.hpp"
#include "util/logger.hpp"

class BufferBuilder {

private:
    std::vector<int> _out;
    int _total_literal_limit;
    int _counter_position;
    BufferIterator _it;
    int _num_added_lits = 0;

public:
    BufferBuilder(int totalLiteralLimit, int maxClauseLength, bool slotsForSumOfLengthAndLbd) :
        _total_literal_limit(totalLiteralLimit), _it(maxClauseLength, slotsForSumOfLengthAndLbd) {

        for (int i = 0; i < sizeof(size_t)/sizeof(int); i++) _out.push_back(0);
        *((size_t*) _out.data()) = 1;
        _counter_position = _out.size();
        _out.push_back(0); // counter for the first group
        if (totalLiteralLimit > 0) _out.reserve(totalLiteralLimit);
    }

    bool append(const Mallob::Clause& c) {

        if (_total_literal_limit >= 0 && _num_added_lits + c.size > _total_literal_limit) 
            return false;


        int numSwitches = 0;
        while (c.size != _it.clauseLength || c.lbd != _it.lbd) {
            numSwitches++;
            _counter_position = _out.size();
            _out.push_back(0); // counter
            _it.nextLengthLbdGroup();
            assert(_it.clauseLength <= 255);
        }

        //LOG(V2_INFO, "APPEND %s (%i)\n", c.toStr().c_str(), numSwitches);

        _out[_counter_position]++;
        assert(c.begin != nullptr);
        _out.insert(_out.end(), c.begin, c.begin+c.size);
        _num_added_lits += c.size;
        return true;
    }

    std::vector<int>&& extractBuffer() {
        return std::move(_out);
    }
};

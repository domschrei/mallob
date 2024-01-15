
#pragma once

#include <vector>

#include "app/sat/data/clause_metadata.hpp"
#include "buffer_iterator.hpp"
#include "../../data/clause.hpp"
#include "util/logger.hpp"

class BufferBuilder {

public:
    struct FailedInsertion {
        int lastCounterPosition;
        BufferIterator lastBucket;
        BufferIterator failedBucket;
    };

private:
    std::vector<int>* _out;
    bool _owning_vector = false;

    int _total_literal_limit;
    int _counter_position;
    BufferIterator _it;
    int _num_added_clauses = 0;
    int _num_added_lits = 0;

    FailedInsertion _failed_insertion;

public:
    BufferBuilder(int totalLiteralLimit, int maxClauseLength, bool slotsForSumOfLengthAndLbd, std::vector<int>* out = nullptr) :
        _out(out), _total_literal_limit(totalLiteralLimit), _it(maxClauseLength, slotsForSumOfLengthAndLbd) {

        if (_total_literal_limit < 0) _total_literal_limit = INT32_MAX;

        if (_out == nullptr) {
            _out = new std::vector<int>();
            _owning_vector = true;
        }

        for (int i = 0; i < sizeof(size_t)/sizeof(int); i++) _out->push_back(0);
        *((size_t*) _out->data()) = 1;
        _counter_position = _out->size();
        _out->push_back(0); // counter for the first group
        if (totalLiteralLimit > 0) _out->reserve(totalLiteralLimit);
    }
    ~BufferBuilder() {
        if (_owning_vector && _out != nullptr) delete _out;
    }

    bool append(const Mallob::Clause& c) {

        if (_total_literal_limit >= 0 && _num_added_lits + c.size-ClauseMetadata::numInts() > _total_literal_limit) {
            // Buffer is full!
            // Assemble some information on the fail
            _failed_insertion.lastCounterPosition = _counter_position;
            _failed_insertion.lastBucket = _it;
            _failed_insertion.failedBucket = _it;
            while (c.size != _failed_insertion.failedBucket.clauseLength 
                    || c.lbd != _failed_insertion.failedBucket.lbd) {
                _failed_insertion.failedBucket.nextLengthLbdGroup();
            }
            return false;
        }

        //LOG(V2_INFO, "APPEND %s\n", c.toStr().c_str());

        int numSwitches = 0;
        while (c.size != _it.clauseLength || c.lbd != _it.lbd) {
            numSwitches++;
            _counter_position = _out->size();
            _out->push_back(0); // counter
            _it.nextLengthLbdGroup();
            assert(_it.clauseLength <= 255);
        }

        (*_out)[_counter_position]++;
        assert(c.begin != nullptr);
        _out->insert(_out->end(), c.begin, c.begin+c.size);
        _num_added_lits += c.size - ClauseMetadata::numInts();
        _num_added_clauses++;
        return true;
    }

    int getCurrentCounterPosition() const {
        return _counter_position;
    }

    FailedInsertion getFailedInsertionInfo() const {
        return _failed_insertion;
    }

    int getNumAddedClauses() const {
        return _num_added_clauses;
    }

    int getNumAddedLits() const {
        return _num_added_lits;
    }

    inline int getMaxRemainingLits() const {
        return _total_literal_limit - _num_added_lits;
    }

    std::vector<int>&& extractBuffer() {
        return std::move(*_out);
    }
};

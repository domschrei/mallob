
#include <algorithm>

#include "buffer_merger.hpp"

BufferMerger::BufferMerger(int sizeLimit, int maxClauseLength, bool slotsForSumOfLengthAndLbd, bool useChecksum) : 
    _size_limit(sizeLimit), _max_clause_length(maxClauseLength), 
    _slots_for_sum_of_length_and_lbd(slotsForSumOfLengthAndLbd), _use_checksum(useChecksum) {}

void BufferMerger::add(BufferReader&& reader) {_readers.push_back(std::move(reader));}

std::vector<int> BufferMerger::merge(std::vector<int>* excessClauses) {
    
    AbstractClauseThreewayComparator* threewayCompare = _slots_for_sum_of_length_and_lbd ?
        (AbstractClauseThreewayComparator*) new LengthLbdSumClauseThreewayComparator(_max_clause_length+2) :
        (AbstractClauseThreewayComparator*) new LexicographicClauseThreewayComparator();
    ClauseComparator compare(threewayCompare);
    InputClauseComparator inputCompare(threewayCompare);

    // Setup readers
    for (size_t i = 0; i < _readers.size(); i++) {

        // Fetch first clause of this reader
        Clause* c = _readers[i].getCurrentClausePointer();
        _readers[i].getNextIncomingClause();
        if (c->begin == nullptr) continue;
        InputClause inputClause(c, i);

        // Insert clause into merger
        if (_merger.empty()) _merger.insert_after(_merger.before_begin(), inputClause);
        else {
            auto it = _merger.before_begin(); 
            auto nextIt = it; ++nextIt;
            while (nextIt != _merger.end() && inputCompare(inputClause, *nextIt)) {
                ++it;
                ++nextIt;
            }
            _merger.insert_after(it, inputClause);
        }
    }

    // Setup builders for main buffer and excess clauses buffer
    BufferBuilder mainBuilder(_size_limit, _max_clause_length, _slots_for_sum_of_length_and_lbd);
    BufferBuilder* excessBuilder;
    if (excessClauses != nullptr) {
        excessBuilder = new BufferBuilder(_size_limit, _max_clause_length, _slots_for_sum_of_length_and_lbd);
    }
    BufferBuilder* currentBuilder = &mainBuilder;

    // For checking duplicates
    Clause lastSeenClause;

    // Merge rounds
    while (!_merger.empty()) {

        // Fetch next best clause
        auto& [clause, readerId] = _merger.front();
        
        // Duplicate?
        if (lastSeenClause.begin == nullptr || compare(lastSeenClause, *clause)) {
            // -- not a duplicate
            lastSeenClause = *clause;

            // Try to append to current builder
            bool success = currentBuilder->append(lastSeenClause);
            if (!success && currentBuilder == &mainBuilder) {
                // Switch from normal output to excess clauses output
                currentBuilder = excessBuilder;
                success = currentBuilder->append(lastSeenClause);
            }
        } else {
            // Duplicate!
            assert(!compare(*clause, lastSeenClause) || 
                log_return_false("ERROR: Clauses unordered - %s <-> %s\n", 
                clause->toStr().c_str(), lastSeenClause.toStr().c_str()));
        }

        // Refill merger
        _readers[readerId].getNextIncomingClause();
        if (clause->begin == nullptr) {
            // No clauses left for this reader
            _merger.erase_after(_merger.before_begin());
        } else {
            // Insert clause at the correct position in the merger
            auto it = _merger.begin(); 
            auto nextIt = it; ++nextIt;
            while (nextIt != _merger.end() && inputCompare(_merger.front(), *nextIt)) {
                ++it;
                ++nextIt;
            }
            if (it != _merger.begin()) {
                // Move element
                auto elem = _merger.front();
                _merger.erase_after(_merger.before_begin());
                _merger.insert_after(it, elem);
            } // Else: element is already at the right place
        }
    }

    // Fill provided excess clauses buffer with result from according builder
    if (excessClauses != nullptr) {
        *excessClauses = excessBuilder->extractBuffer();
        delete excessBuilder;
    }

    delete threewayCompare;
    return mainBuilder.extractBuffer();
}

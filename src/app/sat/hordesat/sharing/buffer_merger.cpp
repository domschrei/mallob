
#include <algorithm>

#include "buffer_merger.hpp"

BufferMerger::BufferMerger(int maxLbdPartitionedSize, bool useChecksum) : 
    _max_lbd_partitioned_size(maxLbdPartitionedSize), _use_checksum(useChecksum) {}

void BufferMerger::add(BufferReader&& reader) {_readers.push_back(std::move(reader));}

std::vector<int> BufferMerger::merge(int sizeLimit, std::vector<int>* excessClauses) {
    
    // New implementation, faster in most cases (especially many short clauses).
    return fastMerge(sizeLimit, excessClauses);
    
    // Legacy code of old implementation follows.

    _clause_filter = ExactSortedClauseFilter();
    _next_clauses.resize(_readers.size());
    _selected.resize(_readers.size());
    _hash = 1;

    //int numReceived[_readers.size()];
    //int numAdded[_readers.size()];
    //int numFiltered[_readers.size()];
    for (size_t i = 0; i < _readers.size(); i++) {
        _next_clauses[i].begin = nullptr;
        _next_clauses[i].size = 0;
        _selected[i] = false;
        //numReceived[i] = 0;
        //numAdded[i] = 0;
        //numFiltered[i] = 0;
    }

    //LOG(V4_VVER, "MERGE setup\n");

    std::vector<int> out;
    out.reserve(sizeLimit);
    for (int i = 0; i < sizeof(size_t)/sizeof(int); i++)
        out.push_back(0); // placeholder for checksum
    out.push_back(0); // counter for clauses of first bucket

    _bucket = BucketLabel();
    _counter_pos = out.size()-1;

    //LOG(V4_VVER, "MERGE begin\n");

    // Merge as many clauses as possible into the out buffer
    int numRounds = 0;
    while (mergeRound(out, sizeLimit)) {numRounds++;}
    memcpy(out.data(), &_hash, sizeof(size_t));

    //LOG(V4_VVER, "MERGE rounds=%i\n", numRounds);

    // If requested, merge all excess (but unique) clauses
    // into the excess buffer
    if (excessClauses != nullptr) {
        
        // prepare buffer
        excessClauses->clear();
        for (int i = 0; i < sizeof(size_t)/sizeof(int); i++)
            excessClauses->push_back(0); // placeholder for checksum
        excessClauses->push_back(0); // counter for clauses of current bucket
        
        _bucket = BucketLabel();
        _counter_pos = excessClauses->size()-1;
        _hash = 1;
        
        // merge remaining clauses
        while (mergeRound(*excessClauses, INT32_MAX)) {}
        memcpy(excessClauses->data(), &_hash, sizeof(size_t));

        // Remove trailing zeroes
        size_t lastNonzeroIdx = excessClauses->size()-1;
        while (lastNonzeroIdx > 0 && excessClauses->at(lastNonzeroIdx) == 0) lastNonzeroIdx--;
        excessClauses->resize(lastNonzeroIdx+1);
    }

    //LOG(V4_VVER, "MERGE end\n");

    return out;
}

std::vector<int> BufferMerger::fastMerge(int sizeLimit, std::vector<int>* excessClauses) {
    
    _hash = 1;
    InputClauseComparator inputCompare;

    for (size_t i = 0; i < _readers.size(); i++) {

        Clause* c = _readers[i].getCurrentClausePointer();
        _readers[i].getNextIncomingClause();
        if (c->begin == nullptr) continue;
        InputClause inputClause(c, i);

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

    //LOG(V4_VVER, "MERGE setup\n");

    std::vector<int> out;
    out.reserve(sizeLimit);
    for (int i = 0; i < sizeof(size_t)/sizeof(int); i++)
        out.push_back(0); // placeholder for checksum
    out.push_back(0); // counter for clauses of first bucket

    if (excessClauses != nullptr) {
        excessClauses->clear();
        for (int i = 0; i < sizeof(size_t)/sizeof(int); i++)
            excessClauses->push_back(0); // placeholder for checksum
        excessClauses->push_back(0); // counter for clauses of first bucket
    }

    _bucket = BucketLabel();
    _counter_pos = out.size()-1;
    std::vector<int>* outputVector = &out;
    bool fillingMainVector = true;

    // For checking duplicates
    Clause lastSeenClause;
    ClauseComparator compare;

    //LOG(V4_VVER, "MERGE begin\n");

    while (!_merger.empty()) {

        //for (auto it = _merger.begin(); it != _merger.end(); ++it) {
        //    auto& [clause, readerId] = *it;
        //    LOG(V5_DEBG, "MERGER reader=%i %s\n", readerId, clause->toStr().c_str());
        //}

        // Fetch next best clause
        auto& [clause, readerId] = _merger.front();

        //log(V5_DEBG, "CLAUSE reader=%i %s\n", readerId, clause.toStr().c_str());
        
        // Duplicate?
        if (lastSeenClause.begin == nullptr || compare(lastSeenClause, *clause)) {
            // -- not a duplicate
            lastSeenClause = *clause;

            // Check if size of buffer would be exceeded with next clause
            if (fillingMainVector && outputVector->size() + clause->size + (clause->size <= _max_lbd_partitioned_size ? 0:1) > sizeLimit) {
                // Size exceeded: finalize vector
                memcpy(out.data(), &_hash, sizeof(size_t));
                if (excessClauses == nullptr) break;
                // Switch from normal output to excess clauses output
                fillingMainVector = false;
                outputVector = excessClauses;
                _bucket = BucketLabel();
                _counter_pos = excessClauses->size()-1;
                _hash = 1;
                size_t remainingSize = 0;
                for (auto& [clause, readerId] : _merger) {
                    remainingSize += _readers[readerId].getRemainingSize();
                }
                outputVector->reserve(remainingSize);
            }
            
            // Update bucket
            while (clause->size > _bucket.size || (clause->size <= _max_lbd_partitioned_size && clause->lbd > _bucket.lbd)) {
                _bucket.next(_max_lbd_partitioned_size);
                outputVector->push_back(0);
                _counter_pos = outputVector->size()-1;
            }

            // Insert clause
            //log(V5_DEBG, "INSERT %s\n", clause.toStr().c_str());
            size_t sizeBefore = outputVector->size();
            if (clause->size > _max_lbd_partitioned_size) outputVector->push_back(clause->lbd);
            if (clause->size == 1) outputVector->push_back(*clause->begin);
            else outputVector->insert(outputVector->end(), clause->begin, clause->begin+clause->size);
            
            if (_use_checksum) {
                hash_combine(_hash, ClauseHasher::hash(
                    outputVector->data()+sizeBefore,
                    clause->size+(clause->size <= _max_lbd_partitioned_size ? 0 : 1), 3
                ));
            }
            outputVector->at(_counter_pos)++;
        } else {
            // Duplicate!
            assert(!compare(*clause, lastSeenClause)); // must be the same
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

    if (fillingMainVector) {
        memcpy(out.data(), &_hash, sizeof(size_t));
        _hash = 1;
    }

    if (excessClauses != nullptr) {
        memcpy(excessClauses->data(), &_hash, sizeof(size_t));
        
        // Remove trailing zeroes
        size_t lastNonzeroIdx = excessClauses->size()-1;
        while (lastNonzeroIdx > 0 && excessClauses->at(lastNonzeroIdx) == 0) lastNonzeroIdx--;
        excessClauses->resize(lastNonzeroIdx+1);
    }

    //LOG(V4_VVER, "MERGE end\n");
    return out;
}


bool BufferMerger::mergeRound(std::vector<int>& out, int sizeLimit) {
    BucketLabel newBucket = _bucket;
    int maxBestIndex = -1;
    
    // Fetch next clauses, get minimum size / lbd
    for (size_t i = 0; i < _readers.size(); i++) {
        Clause& c = _next_clauses[i];
        // no clauses left
        if (c.size == -1) continue;

        // If no clause is present, try to read the next one
        if (c.begin == nullptr) {
            c = _readers[i].getNextIncomingClause();
            if (c.begin == nullptr) {
                // no clauses left, set a magic number
                //log(V5_DEBG, "%i : out of clauses after receiving %i cls (%i added, %i filtered)\n", i, numReceived[i], numAdded[i], numFiltered[i]);
                c.size = -1;
                _selected[i] = false;
                continue;
            }
            //numReceived[i]++;
        }
        
        // Is the clause eligible for selection based on the
        // current bucket and the best clause found so far this round?
        _selected[i] = (maxBestIndex == -1 || c.size < newBucket.size || 
                (c.size == newBucket.size 
                && (newBucket.size > _max_lbd_partitioned_size 
                || c.lbd <= newBucket.lbd)));

        // Does this clause impose a *new* bound on the best clauses this round?
        if (maxBestIndex == -1 || (_selected[i] && (c.size < newBucket.size || 
                (newBucket.size <= _max_lbd_partitioned_size && c.lbd < newBucket.lbd)))) {
            newBucket.size = c.size;
            newBucket.lbd = c.lbd;
            maxBestIndex = i;
        }
    }
    
    // Check if any clauses are left
    if (maxBestIndex == -1) return false;

    bool isLbdPartitioned = newBucket.size <= _max_lbd_partitioned_size;
    
    // Check if size of buffer would be exceeded with next clause
    if (out.size() + newBucket.size + (isLbdPartitioned?0:1) > sizeLimit)
        return false;

    assert(_bucket.size <= newBucket.size
            || LOG_RETURN_FALSE("lbd-partitioned: %s, (%i,%i) => (%i,%i)\n", 
                isLbdPartitioned?"true":"false", _bucket.size, _bucket.lbd, 
                newBucket.size, newBucket.lbd));
    if (_bucket.size == newBucket.size) 
        assert(!isLbdPartitioned || _bucket.lbd <= newBucket.lbd
            || LOG_RETURN_FALSE("lbd-partitioned: %s, (%i,%i) => (%i,%i)\n", 
                isLbdPartitioned?"true":"false", _bucket.size, _bucket.lbd, 
                newBucket.size, newBucket.lbd));

    // Skip empty intermediate buckets
    bool bucketChanged = false;
    while (_bucket.size != newBucket.size || (isLbdPartitioned && _bucket.lbd != newBucket.lbd)) {
        bucketChanged = true;
        _bucket.next(_max_lbd_partitioned_size);
        out.push_back(0);
    }
    if (bucketChanged) {
        _bucket.lbd = newBucket.lbd; // if !isLbdPartitioned
        _counter_pos = out.size()-1;
        // Reset clause filter
        _clause_filter.clear();
    }

    // Insert all currently "best" clauses
    for (size_t i = maxBestIndex; i < _readers.size(); i++) {
        if (!_selected[i]) continue;
        Clause& c = _next_clauses[i];
        assert(c.begin != nullptr);
        assert(c.size > 0);

        // Clause not contained yet?
        if (_clause_filter.registerClause(c)) {

            // Insert this clause
            size_t sizeBefore = out.size();
            if (!isLbdPartitioned) out.push_back(c.lbd);
            out.insert(out.end(), c.begin, c.begin+c.size);
            if (_use_checksum) {
                hash_combine(_hash, ClauseHasher::hash(
                    out.data()+sizeBefore,
                    c.size+(isLbdPartitioned ? 0 : 1), 3
                ));
            }
            out[_counter_pos]++;
            //numAdded[i]++;
        } //else numFiltered[i]++;

        // Reset clause slot
        c.begin = nullptr;
    }

    return true;     
}


#include "buffer_merger.hpp"

BufferMerger::BufferMerger(int maxLbdPartitionedSize, bool useChecksum) : 
    _max_lbd_partitioned_size(maxLbdPartitionedSize), _use_checksum(useChecksum) {}

void BufferMerger::add(BufferReader&& reader) {_readers.push_back(std::move(reader));}

std::vector<int> BufferMerger::merge(int sizeLimit, std::vector<int>* excessClauses) {
    
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

    std::vector<int> out;
    out.reserve(sizeLimit);
    for (int i = 0; i < sizeof(size_t)/sizeof(int); i++)
        out.push_back(0); // placeholder for checksum
    out.push_back(0); // counter for clauses of first bucket

    _bucket = BucketLabel();
    _counter_pos = out.size()-1;

    // Merge as many clauses as possible into the out buffer
    while (mergeRound(out, sizeLimit)) {}
    memcpy(out.data(), &_hash, sizeof(size_t));

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
            || log_return_false("lbd-partitioned: %s, (%i,%i) => (%i,%i)\n", 
                isLbdPartitioned?"true":"false", _bucket.size, _bucket.lbd, 
                newBucket.size, newBucket.lbd));
    if (_bucket.size == newBucket.size) 
        assert(!isLbdPartitioned || _bucket.lbd <= newBucket.lbd
            || log_return_false("lbd-partitioned: %s, (%i,%i) => (%i,%i)\n", 
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

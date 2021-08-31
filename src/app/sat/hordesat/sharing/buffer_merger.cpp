
#include "buffer_merger.hpp"

BufferMerger::BufferMerger(int maxLbdPartitionedSize, bool useChecksum) : 
    _max_lbd_partitioned_size(maxLbdPartitionedSize), _use_checksum(useChecksum) {}

void BufferMerger::add(BufferReader&& reader) {_readers.push_back(std::move(reader));}

std::vector<int> BufferMerger::merge(int sizeLimit) {
    
    ExactSortedClauseFilter _clause_filter;
    Clause nextClauses[_readers.size()];
    bool selected[_readers.size()];
    size_t hash = 1;

    //int numReceived[_readers.size()];
    //int numAdded[_readers.size()];
    //int numFiltered[_readers.size()];
    for (size_t i = 0; i < _readers.size(); i++) {
        nextClauses[i].begin = nullptr;
        nextClauses[i].size = 0;
        //numReceived[i] = 0;
        //numAdded[i] = 0;
        //numFiltered[i] = 0;
    }

    std::vector<int> out;
    out.reserve(sizeLimit);
    for (int i = 0; i < sizeof(size_t)/sizeof(int); i++)
        out.push_back(0); // placeholder for checksum
    out.push_back(0); // counter for clauses of first bucket

    BucketLabel bucket;
    int counterPos = out.size()-1;

    while (true) {
        BucketLabel newBucket = bucket;
        int maxBestIndex = -1;
        
        // Fetch next clauses, get minimum size / lbd
        for (size_t i = 0; i < _readers.size(); i++) {
            Clause& c = nextClauses[i];
            // no clauses left
            if (c.size == -1) continue;

            // If no clause is present, try to read the next one
            if (c.begin == nullptr) {
                c = _readers[i].getNextIncomingClause();
                if (c.begin == nullptr) {
                    // no clauses left, set a magic number
                    //log(V5_DEBG, "%i : out of clauses after receiving %i cls (%i added, %i filtered)\n", i, numReceived[i], numAdded[i], numFiltered[i]);
                    c.size = -1;
                    selected[i] = false;
                    continue;
                }
                //numReceived[i]++;
            }
            
            // Is the clause eligible for selection based on the
            // current bucket and the best clause found so far this round?
            selected[i] = (maxBestIndex == -1 || c.size < newBucket.size || 
                    (c.size == newBucket.size 
                    && (newBucket.size > _max_lbd_partitioned_size 
                    || c.lbd <= newBucket.lbd)));

            // Does this clause impose a *new* bound on the best clauses this round?
            if (maxBestIndex == -1 || (selected[i] && (c.size < newBucket.size || 
                    (newBucket.size <= _max_lbd_partitioned_size && c.lbd < newBucket.lbd)))) {
                newBucket.size = c.size;
                newBucket.lbd = c.lbd;
                maxBestIndex = i;
            }
        }
        
        // Check if any clauses are left
        if (maxBestIndex == -1) break;

        bool isLbdPartitioned = newBucket.size <= _max_lbd_partitioned_size;
        
        // Check if size of buffer would be exceeded with next clause
        if (out.size() + newBucket.size + (isLbdPartitioned?0:1) > sizeLimit)
            break;

        assert(bucket.size <= newBucket.size
                || log_return_false("lbd-partitioned: %s, (%i,%i) => (%i,%i)\n", 
                    isLbdPartitioned?"true":"false", bucket.size, bucket.lbd, 
                    newBucket.size, newBucket.lbd));
        if (bucket.size == newBucket.size) 
            assert(!isLbdPartitioned || bucket.lbd <= newBucket.lbd
                || log_return_false("lbd-partitioned: %s, (%i,%i) => (%i,%i)\n", 
                    isLbdPartitioned?"true":"false", bucket.size, bucket.lbd, 
                    newBucket.size, newBucket.lbd));

        // Skip empty intermediate buckets
        bool bucketChanged = false;
        while (bucket.size != newBucket.size || (isLbdPartitioned && bucket.lbd != newBucket.lbd)) {
            bucketChanged = true;
            bucket.next(_max_lbd_partitioned_size);
            out.push_back(0);
        }
        if (bucketChanged) {
            bucket.lbd = newBucket.lbd; // if !isLbdPartitioned
            counterPos = out.size()-1;
            // Reset clause filter
            _clause_filter.clear();
        }

        // Insert all currently "best" clauses
        for (size_t i = maxBestIndex; i < _readers.size(); i++) {
            if (!selected[i]) continue;
            Clause& c = nextClauses[i];
            assert(c.begin != nullptr);
            assert(c.size > 0);

            // Clause not contained yet?
            if (_clause_filter.registerClause(c)) {

                // Insert this clause
                size_t sizeBefore = out.size();
                if (!isLbdPartitioned) out.push_back(c.lbd);
                out.insert(out.end(), c.begin, c.begin+c.size);
                if (_use_checksum) {
                    hash_combine(hash, ClauseHasher::hash(
                        out.data()+sizeBefore,
                        c.size+(isLbdPartitioned ? 0 : 1), 3
                    ));
                }
                out[counterPos]++;
                //numAdded[i]++;
            } //else numFiltered[i]++;

            // Reset clause slot
            c.begin = nullptr;
        }            
    }

    memcpy(out.data(), &hash, sizeof(size_t));
    return out;
}

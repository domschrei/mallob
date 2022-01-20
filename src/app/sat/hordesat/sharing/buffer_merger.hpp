
#ifndef DOMPASCH_MALLOB_BUFFER_MERGER_HPP
#define DOMPASCH_MALLOB_BUFFER_MERGER_HPP

#include <vector>
#include <set>
#include <forward_list>

#include "buffer_reader.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp" 

class BufferMerger {
public:
    struct ClauseThreewayComparator {
        int compare(const Clause& left, const Clause& right) const {
            if (left.size != right.size) return left.size < right.size ? -1 : 1;
            if (left.lbd != right.lbd) return left.lbd < right.lbd ? -1 : 1;
            for (size_t i = 0; i < left.size; i++) {
                if (left.begin[i] != right.begin[i]) 
                    return left.begin[i] < right.begin[i] ? -1 : 1;
            }
            return 0;
        }
    };
    struct ClauseComparator {
        ClauseThreewayComparator compare;
        bool operator()(const Clause& left, const Clause& right) const {
            return compare.compare(left, right) == -1;
        }
    };
private:
    int _max_lbd_partitioned_size;
    bool _use_checksum;
    std::vector<BufferReader> _readers;

    std::vector<Clause> _next_clauses;
    std::vector<bool> _selected;

    typedef std::pair<Clause*, int> InputClause;
    struct InputClauseComparator {
        ClauseThreewayComparator compare;
        bool operator()(const InputClause& left, const InputClause& right) const {
            int res = compare.compare(*left.first, *right.first);
            if (res != 0) return res > 0;
            if (left.second != right.second) return left.second < right.second;
            return false;
        }
    };
    std::forward_list<InputClause> _merger;

    BucketLabel _bucket;
    int _counter_pos;
    size_t _hash;
    ExactSortedClauseFilter _clause_filter;

public:
    BufferMerger(int maxLbdPartitionedSize, bool useChecksum = false);
    void add(BufferReader&& reader);
    std::vector<int> merge(int sizeLimit, std::vector<int>* excessClauses = nullptr);
    std::vector<int> fastMerge(int sizeLimit, std::vector<int>* excessClauses = nullptr);
    
private:
    bool mergeRound(std::vector<int>& buffer, int sizeLimit);
};

#endif 

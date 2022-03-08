
#ifndef DOMPASCH_MALLOB_BUFFER_MERGER_HPP
#define DOMPASCH_MALLOB_BUFFER_MERGER_HPP

#include <vector>
#include <set>
#include <forward_list>

#include "buffer_builder.hpp"
#include "buffer_reader.hpp"
#include "app/sat/hordesat/utilities/clause_filter.hpp" 

class BufferMerger {
public:
    struct AbstractClauseThreewayComparator {
        virtual int compare(const Clause& left, const Clause& right) const = 0;
    };
    struct LexicographicClauseThreewayComparator : public AbstractClauseThreewayComparator {
        int compare(const Clause& left, const Clause& right) const {
            // Shortest length first
            if (left.size != right.size) return left.size < right.size ? -1 : 1;
            // Shortest LBD first
            if (left.lbd != right.lbd) return left.lbd < right.lbd ? -1 : 1;
            // Lexicographic comparison of literals
            for (size_t i = 0; i < left.size; i++) {
                if (left.begin[i] != right.begin[i]) 
                    return left.begin[i] < right.begin[i] ? -1 : 1;
            }
            return 0;
        }
    };
    struct LengthLbdSumClauseThreewayComparator : public AbstractClauseThreewayComparator {
        int maxLengthLbdSum;
        LengthLbdSumClauseThreewayComparator(int maxLengthLbdSum) : maxLengthLbdSum(maxLengthLbdSum) {}
        int compare(const Clause& left, const Clause& right) const {
            // Shortest sum of length + lbd first
            int leftSum = left.size+left.lbd;
            int rightSum = right.size+right.lbd;
            if (leftSum != rightSum && (leftSum <= maxLengthLbdSum || rightSum <= maxLengthLbdSum)) 
                return left.size+left.lbd < right.size+right.lbd ? -1 : 1;
            // Shortest length first
            if (left.size != right.size) return left.size < right.size ? -1 : 1;
            // Shortest LBD first
            if (left.lbd != right.lbd) return left.lbd < right.lbd ? -1 : 1;
            // Lexicographic comparison of literals
            for (size_t i = 0; i < left.size; i++) {
                if (left.begin[i] != right.begin[i]) 
                    return left.begin[i] < right.begin[i] ? -1 : 1;
            }
            return 0;
        }
    };
    struct ClauseComparator {
        AbstractClauseThreewayComparator* compare;
        ClauseComparator(AbstractClauseThreewayComparator* compare) : compare(compare) {}
        bool operator()(const Clause& left, const Clause& right) const {
            return compare->compare(left, right) == -1;
        }
    };
private:
    int _size_limit;
    int _max_clause_length;
    int _slots_for_sum_of_length_and_lbd;

    bool _use_checksum;
    std::vector<BufferReader> _readers;

    std::vector<Clause> _next_clauses;
    std::vector<bool> _selected;

    typedef std::pair<Clause*, int> InputClause;
    struct InputClauseComparator {
        AbstractClauseThreewayComparator* compare;
        InputClauseComparator(AbstractClauseThreewayComparator* compare) : compare(compare) {}
        bool operator()(const InputClause& left, const InputClause& right) const {
            int res = compare->compare(*left.first, *right.first);
            if (res != 0) return res > 0;
            if (left.second != right.second) return left.second < right.second;
            return false;
        }
    };
    std::forward_list<InputClause> _merger;

public:
    BufferMerger(int sizeLimit, int maxClauseLength, bool slotsForSumOfLengthAndLbd, bool useChecksum = false);
    void add(BufferReader&& reader);
    std::vector<int> merge(std::vector<int>* excessClauses = nullptr);
    
private:
    std::vector<int> fastMerge(int sizeLimit, std::vector<int>* excessClauses = nullptr);
};

#endif 

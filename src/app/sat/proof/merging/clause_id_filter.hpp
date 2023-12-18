
#pragma once

#include "app/sat/proof/lrat_line.hpp"
#include "util/bloom_filter.hpp"
#include "util/tsl/robin_map.h"
#include "util/tsl/robin_set.h"
#include <memory>
class ClauseIdFilter {

public:
    enum Mode {APPROX_BLOOM, EXACT};

private:
    Mode _mode;
    std::unique_ptr<BloomFilter<LratClauseId>> _bloom_filter;
    std::unique_ptr<tsl::robin_set<LratClauseId>> _exact_filter;

public:
    ClauseIdFilter(Mode mode) : _mode(mode) {
        if (_mode == APPROX_BLOOM) {
            // TODO choose size relative to proof size?
            _bloom_filter.reset(new BloomFilter<LratClauseId>(268435399, 4));
        }
        if (_mode == EXACT) {
            _exact_filter.reset(new tsl::robin_set<LratClauseId>());
        }
    }

    bool registerId(LratClauseId id) {
        if (_mode == APPROX_BLOOM) {
            return _bloom_filter->tryInsert(id);
        }
        if (_mode == EXACT) {
            auto it = _exact_filter->find(id);
            if (it != _exact_filter->end()) return false;
            _exact_filter->insert(it, id);
            return true;            
        }
        return false;
    }

    void unregisterId(LratClauseId id) {
        if (_mode == EXACT) {
            _exact_filter->erase(id);
        }
    }
};

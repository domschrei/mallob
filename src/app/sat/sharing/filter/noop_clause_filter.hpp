
#pragma once

#include "app/sat/sharing/filter/generic_clause_filter.hpp"

// This filter does nothing but attempt to add each clause to the clause store directly.
// No clauses are filtered out and only ADMITTED and DROPPED are possible results of
// tryRegisterAndInsert.
class NoopClauseFilter : public GenericClauseFilter {

public:
	NoopClauseFilter(GenericClauseStore& clauseStore) : 
		GenericClauseFilter(clauseStore) {}
	virtual ~NoopClauseFilter() {}

	ExportResult tryRegisterAndInsert(ProducedClauseCandidate&& pcc) override {
		Mallob::Clause c(pcc.begin, pcc.size, pcc.lbd);
		if (_clause_store.addClause(c)) {
			return ADMITTED;
		} else return DROPPED;
	}

    cls_producers_bitset getProducers(Mallob::Clause& c, int epoch) override {
		cls_producers_bitset result = 0;
		return result;
	}
    
	bool admitSharing(Mallob::Clause& c, int epoch) override {
		return true;
	}
    
	size_t size(int clauseLength) const override {
		return 0;
	}
};

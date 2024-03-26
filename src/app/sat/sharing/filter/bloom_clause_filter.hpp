
#pragma once

#include <assert.h>
#include <stddef.h>
#include <algorithm>
#include <bitset>
#include <memory>
#include <vector>

#include "../../data/clause.hpp"
#include "app/sat/data/produced_clause_candidate.hpp"
#include "app/sat/sharing/filter/generic_clause_filter.hpp"
#include "app/sat/sharing/filter/produced_clause_filter_commons.hpp"
#include "app/sat/sharing/store/generic_clause_store.hpp"
#include "util/sys/threading.hpp"
#include "util/tsl/robin_hash.h"
#include "util/tsl/robin_set.h"

/*
Comment D. Schreiber 09/2021

In a Bloom filter, we use k distinct hash functions to set specific bits of a bitset of size m
in order to "register" (query and insert) n elements.

In our case, we set m=26843543 and k=4. Depending on the number n of inserted elements
we get the following false positive probabilities:

n		  	  p
     1'000 	  4.92891e-16
    10'000 	  4.91571e-12
   100'000 	  4.78579e-08
   500'000    2.65734e-05
 1'000'000	  0.00036
10'000'000 	  0.36010

Generally: p = (1 - e^(-k*n/m))^k
Source: https://en.wikipedia.org/wiki/Bloom_filter#Probability_of_false_positives

For a low number of threads (1-16), the number of clauses exported within a second
is not expected to surpass anything near a million, at which point an expected number 
of 360 clauses would be spuriously discarded. At 500'000 clauses (still a high number)
we expect a much lower number of 13 spuriously discarded clauses, and at below 300'000
clauses we expect this for less than a single clause to occur. However, a high thoughput
of clauses does imply that the clause filter needs to be cleaned up quite frequently
or we otherwise need to put up with noticeable false positive rates.

In addition, we can reduce the number of inserted clauses by only registering a learnt
clause in a filter if there is (probably) still space for the clause in the database structure.
*/

//#define NUM_BITS 268435399 // 32MB
#define NUM_BITS 26843543 // 3,2MB

class BloomClauseFilter : public GenericClauseFilter {

private:
	std::vector<std::bitset<NUM_BITS>> _bitsets;
	int _max_eff_clause_length = 0;

	tsl::robin_set<int> _units;
	Mutex _mtx_units;

	size_t _nb_inserted {0};

	const bool _locking;
	std::vector<std::unique_ptr<Mutex>> _locks;

public:
	BloomClauseFilter(GenericClauseStore& clauseStore, int nbSolvers, int maxEffClauseLength, bool locking) :
		GenericClauseFilter(clauseStore), _bitsets(nbSolvers), _max_eff_clause_length(maxEffClauseLength), _locking(locking) {

		if (_locking) {
			_locks.resize(maxEffClauseLength+1);
			for (size_t i = 0; i < _locks.size(); i++) _locks[i].reset(new Mutex());
		}
	}
	virtual ~BloomClauseFilter() {}

	ExportResult tryRegisterAndInsert(ProducedClauseCandidate&& c) override {
		return registerClause(Mallob::Clause(c.begin, c.size, c.lbd), c.producerId);
	}

    cls_producers_bitset confirmSharingAndGetProducers(Mallob::Clause& c, int epoch) override {
		cls_producers_bitset result = 0;
		for (int i = 0; i < _bitsets.size(); i++) {
			if (!admitClause(c, i)) result |= (1 << i);
		}
		return result;
	}
    
	bool admitSharing(Mallob::Clause& c, int epoch) override {
		return true;
	}
    
	size_t size(int clauseLength) const override {
		if (clauseLength == 0) return _nb_inserted;
		return 0;
	}

	virtual bool tryAcquireLock(int clauseLength = 0) override {
		if (!_locking) return true;
		return _locks[clauseLength]->tryLock();
	}
	virtual void acquireLock(int clauseLength = 0) override {
		if (!_locking) return;
		_locks[clauseLength]->lock();
	}
	virtual void releaseLock(int clauseLength = 0) override {
		if (!_locking) return;
		_locks[clauseLength]->unlock();
	}
	virtual void acquireAllLocks() override {
		if (!_locking) return;
		std::vector<bool> slotLocked(_locks.size(), false);
		int nbLocked = 0;
		// Repeatedly cycle over the slots, acquiring locks where possible,
		// until all locks are held
		while (nbLocked < _locks.size()) {
			for (size_t i = 0; i < _locks.size(); i++) {
				if (slotLocked[i]) continue;
				if (nbLocked+1 == _locks.size()) {
					// Last slot: acquire lock directly
					acquireLock(i);
					nbLocked++;
					slotLocked[i] = true;
				} else if (tryAcquireLock(i)) {
					nbLocked++;
					slotLocked[i] = true;
				}
			}
		}
	}
	virtual void releaseAllLocks() override {
		if (!_locking) return;
		for (auto& lock : _locks) lock->unlock();
	}

private:
	ExportResult registerClause(const Mallob::Clause& c, int producerId) {
		
		if (!admitClause(c, producerId)) return FILTERED;
		
		if (_clause_store.addClause(c)) {
			_nb_inserted++;
			return ADMITTED;
		} else return DROPPED;
	}

	bool admitClause(const Mallob::Clause& c, int producerId) {
		// Block clauses above maximum length
		if (_max_eff_clause_length > 0 && c.size > _max_eff_clause_length) return false;

		// unit clauses are checked explicitly
		if (c.size == 1) { // Unit clause!

			// Always admit unit clause if a check is not possible right now
			if (!_mtx_units.tryLock()) return true;
			
			int firstLit = c.begin[0];
			bool admit = !_units.count(firstLit);
			if (admit) _units.insert(firstLit);

			_mtx_units.unlock();
			return admit;
		}

		size_t h1 = Mallob::ClauseHasher::hash(c.begin, c.size, 1) % NUM_BITS;
		size_t h2 = Mallob::ClauseHasher::hash(c.begin, c.size, 2) % NUM_BITS;
		size_t h3 = Mallob::ClauseHasher::hash(c.begin, c.size, 3) % NUM_BITS;
		size_t h4 = Mallob::ClauseHasher::hash(c.begin, c.size, 4) % NUM_BITS;

		//std::string lits;
		//for (size_t i = 0; i < size; i++) lits += std::to_string(begin[i]) + ",";
		//lits = lits.substr(0, lits.size()-1);

		assert(producerId >= 0 && producerId < _bitsets.size());
		auto& bitset = _bitsets.at(producerId);

		if (bitset.test(h1) && bitset.test(h2) && bitset.test(h3) && bitset.test(h4)) {
			//LOG(V2_INFO, "%s %i %lu %lu %lu %lu FILTERED\n", lits.c_str(), producerId, h1, h2, h3, h4);
			return false;
		} else {
			bitset.set(h1, true);
			bitset.set(h2, true);
			bitset.set(h3, true);
			bitset.set(h4, true);
			//LOG(V2_INFO, "%s %i %lu %lu %lu %lu ADMITTED\n", lits.c_str(), producerId, h1, h2, h3, h4);
			return true;
		}
	}

};

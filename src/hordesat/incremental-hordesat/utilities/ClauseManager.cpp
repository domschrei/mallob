/*
 * ClauseManager.cpp
 *
 *  Created on: May 27, 2015
 *      Author: balyo
 */

#include "ClauseManager.h"
#include "ClauseFilter.h"
#include <string.h>
#include "Logger.h"
#include "DebugUtils.h"

// the number of bits in a size_t
static const size_t width = sizeof(int) << 3;

void setBit(int* arr, size_t bit) {
	int mask = 1 << (bit % width);
	arr[bit/width] |= mask;
}

void unsetBit(int* arr, size_t bit) {
	int mask = 1 << (bit % width);
	arr[bit/width] &= (~mask);
}

bool testBit(const int* arr, size_t bit) {
	int mask = 1 << (bit % width);
	bool b = (arr[bit/width] & mask) != 0;
	return b;
}

size_t countDiffs(const int* arr1, const int* arr2, const size_t size) {
	size_t diffs = 0;
	for (size_t i = 0; i < size; i++) {
		unsigned int x = arr1[i] ^ arr2[i];
		// count the set bits in x (Brian Kernighan's way)
		for (; x; diffs++) {
			x &= x - 1;
		}
	}
	return diffs;
}


ClauseManager::ClauseManager(size_t hotRounds, size_t warmRounds, size_t signatureSize, size_t clauseBufferSize)
	:hotRounds(hotRounds), warmRounds(warmRounds), signatureSize(signatureSize),
	 signatureBits(signatureSize * (sizeof(int)<<3)), clauseBufferSize(clauseBufferSize),
	 currentRound(0), clausesCount(0) {
	signature = new int[signatureSize];
	memset(signature, 0, sizeof(int)*signatureSize);
	log(0, "Hot rounds limit %lu warm rounds limit %lu\n", hotRounds, warmRounds);
}

size_t ClauseManager::getSignatureDiffCount(const int* signature) {
	return countDiffs(signature, this->signature, signatureSize);
}


bool ClauseManager::addClause(const vector<int>& cls) {
	// special treatment for short clauses
	if (cls.size() < VIP_SIZE_LIMIT) {
		//printVector(cls);
		clsLock.lock();
		AgedClause ac;
		ac.cls = cls;
		ac.bornRound = currentRound;
		size_t csig = ClauseFilter::commutativeHashFunction(cls, 1);
		ac.signature = csig;
		setBit(signature, csig % signatureBits);
		vipClauses.push_back(ac);
		clausesCount++;
		clsLock.unlock();
		return true;
	}
	// standard treatment
	if (clsLock.tryLock() == false) {
		return false;
	}
	size_t csig = ClauseFilter::commutativeHashFunction(cls, 1);
	if (testBit(signature, csig % signatureBits)) {
		clsLock.unlock();
		return false;
	}
	AgedClause ac;
	ac.cls = cls;
	ac.bornRound = currentRound;
	ac.signature = csig;
	clauses.push_back(ac);
	setBit(signature, csig % signatureBits);
	clausesCount++;
	clsLock.unlock();
	return true;
}

int ClauseManager::importClauses(const int* clauseBuffer,
		vector<vector<int> >& newClauses, vector<vector<int> >& vipClauses) {
	int clauses = clauseBuffer[0];
	int seenClauses = 0;
	int addedClauses = 0;
	vector<int> cls;
	int i = 1;
	while (seenClauses < clauses) {
		if (clauseBuffer[i] == 0) {
			if (addClause(cls)) {
				if (cls.size() < VIP_SIZE_LIMIT) {
					vipClauses.push_back(cls);
				} else {
					newClauses.push_back(cls);
				}
				addedClauses++;
			}
			seenClauses++;
			cls.clear();
		} else {
			cls.push_back(clauseBuffer[i]);
		}
		i++;
	}
	return addedClauses;
}

void ClauseManager::filterHot(int* exportClauses, const int* signature) {
	clsLock.lock();
	int pos = 1;
	int added = 0;
	size_t hots = 0;
	size_t warms = 0;
	size_t totalLength = 0;

	// VIP clauses
	for (list<AgedClause>::iterator it = vipClauses.begin(); it != vipClauses.end();) {
		size_t age = currentRound - it->bornRound;
		// delete cold
		if (age > 2*warmRounds) {
			it = vipClauses.erase(it);
			clausesCount--;
		} else {
			warms++;
			if (age < 2*hotRounds) {
				hots++;
			}
			if (age < 2*hotRounds && it->cls.size() + 1 + pos < clauseBufferSize
					&& !testBit(signature, it->signature % signatureBits)) {
				for (size_t i = 0; i < it->cls.size(); i++) {
					exportClauses[pos] = it->cls[i];
					pos++;
				}
				totalLength += it->cls.size();
				exportClauses[pos] = 0;
				pos++;
				added++;
			}
			it++;
		}
	}

	// Standard clauses
	for (list<AgedClause>::iterator it = clauses.begin(); it != clauses.end();) {
		size_t age = currentRound - it->bornRound;
		// delete cold
		if (age > warmRounds) {
			unsetBit(this->signature, it->signature % signatureBits);
			it = clauses.erase(it);
			clausesCount--;
		} else {
			warms++;
			if (age < 2*hotRounds) {
				hots++;
			}
			if (age < hotRounds && it->cls.size() + 1 + pos < clauseBufferSize
					&& !testBit(signature, it->signature % signatureBits)) {
				for (size_t i = 0; i < it->cls.size(); i++) {
					exportClauses[pos] = it->cls[i];
					pos++;
				}
				totalLength += it->cls.size();
				exportClauses[pos] = 0;
				pos++;
				added++;
			}
			it++;
		}
	}
	exportClauses[0] = added;
	clsLock.unlock();
	log(2, "ClauseManager: round %lu all-clauses %lu warm %lu hot %lu, shared-clauses %d, avg len %f\n",
			currentRound, clausesCount, warms, hots, added, totalLength/(float)added);
}

size_t ClauseManager::getClauseCount() {
	return clausesCount;
}

void ClauseManager::nextRound() {
	currentRound++;
}

void ClauseManager::getSignature(int* buffer) {
	memcpy(buffer, signature, sizeof(int)*signatureSize);
}

ClauseManager::~ClauseManager() {
	delete[] signature;
}


/*
 * ClauseManager.h
 *
 *  Created on: May 27, 2015
 *      Author: balyo
 */

#ifndef CLAUSEMANAGER_H_
#define CLAUSEMANAGER_H_

#include <vector>
#include <list>
#include "Threading.h"

using namespace std;

#define VIP_SIZE_LIMIT 4

struct AgedClause {
	vector<int> cls;
	size_t bornRound;
	size_t signature;
};

void setBit(int* arr, size_t bit);
void unsetBit(int* arr, size_t bit);
bool testBit(const int* arr, size_t bit);
size_t countDiffs(const int* arr1, const int* arr2, const size_t size);

class ClauseManager {

private:
	const size_t hotRounds, warmRounds, signatureSize, signatureBits, clauseBufferSize;
	int* signature;
	size_t currentRound;
	size_t clausesCount;
	list<AgedClause> clauses;
	list<AgedClause> vipClauses;
	Mutex clsLock;

public:
	ClauseManager(size_t hotRounds, size_t warmRounds, size_t signatureSize, size_t clauseBufferSize);
	bool addClause(const vector<int>& cls);
	int importClauses(const int* clauseBuffer, vector<vector<int> >& newClauses, vector<vector<int> >& vipClauses);
	size_t getClauseCount();
	size_t getSignatureDiffCount(const int* signature);
	void nextRound();
	void getSignature(int* buffer);
	void filterHot(int* exportClauses, const int* signature);
	virtual ~ClauseManager();
};

#endif /* CLAUSEMANAGER_H_ */

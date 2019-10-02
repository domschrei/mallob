/*
 * ClauseDatabase.h
 *
 *  Created on: Aug 27, 2014
 *      Author: balyo
 */

#ifndef CLAUSEDATABASE_H_
#define CLAUSEDATABASE_H_

#include <vector>
#include "Threading.h"

using namespace std;

#define BUCKET_SIZE 1000

struct Bucket {
	int data[BUCKET_SIZE];
	unsigned int top;
};

class ClauseDatabase {
public:
	virtual ~ClauseDatabase();

	/**
	 * Add a learned clause that you want to share. Return a pointer to it
	 */
	int* addClause(vector<int>& clause);
	/**
	 * Add a very important learned clause that you want to share
	 */
	void addVIPClause(vector<int>& clause);
	/**
	 * Fill the given buffer with data for the sending our learned clauses
	 * Return the number of used memory, at most size.
	 */
	unsigned int giveSelection(int* buffer, unsigned int size, int* selectedCount = NULL);
	/**
	 * Set the pointer for the buffer containing the incoming shared clauses which
	 * is a concatenation of the data returned by the giveSelection method for all the
	 * nodes. Each part has "size" integers.
	 */
	void setIncomingBuffer(const int* buffer, int size, int nodes, int thisNode);
	/**
	 * Fill the given clause with the literals of the next VIP clause.
	 * Return false if no more VIP clauses.
	 */
	bool getNextIncomingVIPClause(vector<int>& clause);
	/**
	 * Fill the given clause with the literals of the next incomming clause.
	 * Return false if no more clauses.
	 */
	bool getNextIncomingClause(vector<int>& clause);

private:
	Mutex addClauseLock;
	const int* incommingBuffer;
	unsigned int size, nodes, thisNode;
	unsigned int lastVipClsIndex, lastVipNode;
	unsigned int lastClsSize, lastClsIndex, lastClsNode, lastClsCount;

	vector<Bucket*> buckets;
	vector<vector<int> > vipClauses;
};

#endif /* CLAUSEDATABASE_H_ */

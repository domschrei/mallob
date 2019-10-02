/*
 * ClauseDatabase.cpp
 *
 *  Created on: Aug 27, 2014
 *      Author: balyo
 */

#include "ClauseDatabase.h"
#include <string.h>
#include <stdio.h>
#include "DebugUtils.h"
#include "Logger.h"


void ClauseDatabase::addVIPClause(vector<int>& clause) {
	addClauseLock.lock();
	vipClauses.push_back(clause);
	addClauseLock.unlock();
}

int* ClauseDatabase::addClause(vector<int>& clause) {
	if (addClauseLock.tryLock() == false) {
		return NULL;
	}

	unsigned int csize = clause.size();

	while (buckets.size() < csize) {
		Bucket* b = new Bucket();
		buckets.push_back(b);
	}
	Bucket* b = buckets[csize-1];
	unsigned int top = b->top;
	if (top + csize < BUCKET_SIZE) {
		// copy the clause
		for (unsigned int i = 0; i < csize; i++) {
			b->data[top + i] = clause[i];
		}
		// update top
		b->top += csize;
		addClauseLock.unlock();
		return b->data + top;
	} else {
		addClauseLock.unlock();
		return NULL;
	}
}

/**
 * Format of the data in the buffer:
 * pos 0: the total length of all VIP clauses (with separators)
 * pos 1 .. k: the VIP clause literals separated by zeros
 * pos k+1: the number of unary clauses
 * pos k+2 .. l: the literals of unary clauses (without separators)
 * pos l+1: the number of binary clauses
 * pos l+2 .. m: the literals of binary clauses (without separators)
 * ...
 * until size ints are used.
 */
unsigned int ClauseDatabase::giveSelection(int* buffer, unsigned int size, int* selectedCount) {
	// lock clause adding
	addClauseLock.lock();
	// clear the buffer
	memset(buffer, 0, sizeof(int)*size);
	unsigned int used = 0;
	// The first value is the total length of VIP clauses (with separators)
	used++
	// First add the VIP clauses
	DEBUG(printf("adding the %d VIP clauses.\n", vipClauses.size()));
	while (!vipClauses.empty()) {
		vector<int>& vipCls = vipClauses.back();
		int len = vipCls.size();
		for (int i = 0; i < len; i++) {
			buffer[used++] = vipCls[i];
		}
		buffer[used++] = 0;
		vipClauses.pop_back();
	}
	buffer[0] = used-1;

	if (used >= size) {
		printf("ERROR: vip clauses exceeded the buffer size.");
		exit(99);
	}

	int fitting = 0;
	int notFitting = 0;

	// The other clauses
	for (unsigned int s = 0; s < buckets.size(); s++) {
		Bucket* b = buckets[s];
		unsigned int left = size - used;
		if (b->top < left) {
			// Bucket nr. s has clauses of length s+1
			DEBUG(printf("will copy all %d cls of length %d\n", b->top/(s+1), s+1);)
			fitting += b->top/(s+1);
			buffer[used++] = b->top/(s+1);
			memcpy(buffer + used, b->data, sizeof(int)*b->top);
			used += b->top;
		} else if (left > s+1) {
			// If at least one clause can be copied (including the number of clauses)
			// Bucket nr. s has clauses of length s+1
			int copy = ((left-1)/(s+1))*(s+1);
			buffer[used++] = copy/(s+1);
			DEBUG(printf("will copy %d cls of length %d\n", copy / (s+1), s+1);)
			memcpy(buffer + used, b->data, sizeof(int)*copy);
			used += copy;
			fitting += copy/(s+1);
			notFitting += (b->top - copy)/(s+1);
		}
		b->top = 0;
	}
	addClauseLock.unlock();
	int all = fitting + notFitting;
	if (all > 0) {
		log(2, "%d fit %d (%d%%) didn't \n", fitting, notFitting, notFitting*100/(all));
	} else {
		log(2, "No clauses for export.\n");
	}
	if (selectedCount != NULL) {
		*selectedCount = fitting;
	}
	return used;
}

void ClauseDatabase::setIncomingBuffer(const int* buffer, int size, int nodes, int thisNode) {
	incommingBuffer = buffer;
	this->size = size;
	this->nodes = nodes;
	this->thisNode = thisNode;
	lastVipClsIndex = 1;
	lastVipNode = 0;

	lastClsNode = 0;
	lastClsSize = 1;
	lastClsIndex = buffer[0]+2;
	lastClsCount = buffer[buffer[0]+1];
}

bool ClauseDatabase::getNextIncomingClause(vector<int>& cls) {
	int offset = lastClsNode*size;
	nodesCycle:
	while (lastClsIndex + lastClsSize >= size || lastClsNode == thisNode) {
		lastClsNode++;
		if (lastClsNode >= nodes) {
			return false;
		}
		offset += size;
		lastClsSize = 1;

		lastClsIndex = incommingBuffer[offset] + 2; //nr. of VIP clauses + 1
		lastClsCount = incommingBuffer[offset + incommingBuffer[offset] + 1];
	}
	while (lastClsCount == 0) {
			lastClsSize++;
			if (lastClsIndex + lastClsSize >= size) {
				goto nodesCycle;
			}
			lastClsCount = incommingBuffer[offset + lastClsIndex];
			lastClsIndex++;
	}
	if (lastClsIndex + lastClsSize <= size) {
		cls.clear();
		for (unsigned int i = 0; i < lastClsSize; i++) {
			cls.push_back(incommingBuffer[offset + lastClsIndex]);
			lastClsIndex++;
		}
		lastClsCount--;
		return true;
	} else {
		return false;
	}
}

bool ClauseDatabase::getNextIncomingVIPClause(vector<int>& cls) {
	unsigned int offset = lastVipNode*size;
	unsigned int vipsHere = incommingBuffer[offset];
	while (lastVipClsIndex > vipsHere) {
		lastVipNode++;
		offset = lastVipNode*size;
		vipsHere = incommingBuffer[offset];
		lastVipClsIndex = 1;
		if (lastVipNode >= nodes) {
			return false;
		}
	}
	cls.clear();
	while (incommingBuffer[offset + lastVipClsIndex] != 0) {
		cls.push_back(incommingBuffer[offset + lastVipClsIndex]);
		lastVipClsIndex++;
	}
	lastVipClsIndex++;
	return true;
}


ClauseDatabase::~ClauseDatabase() {
	for (unsigned int i = 0; i < buckets.size(); i++) {
		delete buckets[i];
	}
}


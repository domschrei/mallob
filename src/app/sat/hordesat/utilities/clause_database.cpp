/*
 * ClauseDatabase.cpp
 *
 *  Created on: Aug 27, 2014
 *      Author: balyo
 */

#include <string.h>
#include <stdio.h>
#include "util/assert.hpp"

#include "clause_database.hpp"
#include "app/sat/hordesat/utilities/debug_utils.hpp"

void ClauseDatabase::addVIPClause(std::vector<int>& clause) {
	auto lock = addClauseLock.getLock();
	vipClauses.push_back(clause);
}

int* ClauseDatabase::addClause(const int* clause, size_t csize) {
	if (addClauseLock.tryLock() == false) return NULL;

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
	used++;
	// First add the VIP clauses
	HORDE_DBG(printf("adding the %d VIP clauses.\n", vipClauses.size()));
	while (!vipClauses.empty()) {
		std::vector<int>& vipCls = vipClauses.back();
		int len = vipCls.size();
		if (used+len+1 >= size) break; // stop if buffer is too small
		for (int i = 0; i < len; i++) {
			buffer[used++] = vipCls[i];
		}
		buffer[used++] = 0;
		vipClauses.pop_back();
	}
	buffer[0] = used-1;

	if (used >= size) {
		LOGGER(logger, V0_CRIT, "ERROR: vip clauses exceeded the buffer size.");
		addClauseLock.unlock();
		*selectedCount = 0;
		return used;
	}

	int fitting = 0;
	int notFitting = 0;

	// The other clauses
	for (unsigned int s = 0; s < buckets.size(); s++) {
		Bucket* b = buckets[s];
		unsigned int left = size - used;
		if (b->top < left) {
			// Bucket nr. s has clauses of length s+1
			HORDE_DBG(printf("will copy all %d cls of length %d\n", b->top/(s+1), s+1);)
			fitting += b->top/(s+1);
			buffer[used++] = b->top/(s+1);
			memcpy(buffer + used, b->data, sizeof(int)*b->top);
			used += b->top;
		} else if (left > s+1) {
			// If at least one clause can be copied (including the number of clauses)
			// Bucket nr. s has clauses of length s+1
			int copy = ((left-1)/(s+1))*(s+1);
			buffer[used++] = copy/(s+1);
			HORDE_DBG(printf("will copy %d cls of length %d\n", copy / (s+1), s+1);)
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
		LOGGER(logger, V5_DEBG, "%d fit %d (%d%%) didn't \n", fitting, notFitting, notFitting*100/(all));
	} else {
		LOGGER(logger, V5_DEBG, "no clauses\n");
	}
	if (selectedCount != NULL) {
		*selectedCount = fitting;
	}
	return used;
}

void ClauseDatabase::setIncomingBuffer(int* buffer, int size) {
	incomingBuffer = buffer;
	bufferSize = size;
	currentPos = 1;
	currentSize = 0; // magic number for vip clauses
	remainingClsOfCurrentSize = 0;
	remainingVipLits = buffer[0]; // # VIP literals including separators
}

int* ClauseDatabase::getNextIncomingClause(int& size) {

	if (remainingVipLits > 0) {
		// VIP clause
		int start = currentPos++;
		while (incomingBuffer[currentPos] != 0) currentPos++;
		// currentPos is now at separator-"0" after the clause to export
		int* clsbegin = incomingBuffer+start;
		size = currentPos-start;

		// Move currentPos pointer to begin of next clause
		currentPos++;
		remainingVipLits--;
		return clsbegin;
	}

	// Find appropriate clause size
	while (remainingClsOfCurrentSize == 0) {
		
		// No more clauses?
		if (currentPos >= bufferSize) return nullptr;

		// Go to next clause size
		currentSize++;
		remainingClsOfCurrentSize = incomingBuffer[currentPos++];
	}
	if (remainingClsOfCurrentSize <= 0) {
		LOGGER(logger, V0_CRIT, "ERROR: %i remaining clauses of size %i at position %i\nBuffer:", remainingClsOfCurrentSize, currentSize, currentPos);
		std::string buffer;
		for (size_t i = 0; i < bufferSize; i++) {
			buffer += " " + std::to_string(incomingBuffer[i]);
		}
		LOGGER(logger, V0_CRIT, "%s\n", buffer.c_str());
		logger.flush();
        abort();
	}

	// Get start and stop index of next clause
	int start = currentPos;
	int stop = start + currentSize;
	currentPos = stop;
	remainingClsOfCurrentSize--;

	// Insert clause literals
	//cls.clear();
	//cls.insert(cls.end(), incomingBuffer+start, incomingBuffer+stop);
	int* clsbegin = incomingBuffer+start;
	size = currentSize;
	return clsbegin;
}

ClauseDatabase::~ClauseDatabase() {
	for (unsigned int i = 0; i < buckets.size(); i++) {
		delete buckets[i];
	}
}


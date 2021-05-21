/*
 * ClauseDatabase.h
 *
 *  Created on: Aug 27, 2014
 *      Author: balyo
 */

#ifndef CLAUSEDATABASE_H_
#define CLAUSEDATABASE_H_

#include <vector>

#include "util/sys/threading.hpp"
#include "util/logger.hpp"

#define BUCKET_SIZE 1000

struct Bucket {
	int data[BUCKET_SIZE];
	unsigned int top;
};

class ClauseDatabase {
public:
	ClauseDatabase(const Logger& logger) : logger(logger) {}
	virtual ~ClauseDatabase();

	/**
	 * Add a learned clause that you want to share. Return a pointer to it
	 */
	int* addClause(const int* clause, size_t size);
	/**
	 * Add a very important learned clause that you want to share
	 */
	void addVIPClause(std::vector<int>& clause);
	/**
	 * Fill the given buffer with data for the sending our learned clauses
	 * Return the number of used memory, at most size.
	 */
	unsigned int giveSelection(int* buffer, unsigned int size, int* selectedCount = NULL);
	/**
	 * Set the pointer for the buffer of <size> containing the incoming shared clauses 
	 * which has the same shape as the data returned by the giveSelection method.
	 */
	void setIncomingBuffer(int* buffer, int size);
	/**
	 * Fill the given clause with the literals of the next incomming clause.
	 * Return false if no more clauses.
	 */
	int* getNextIncomingClause(int& size);

private:
	const Logger& logger;
	Mutex addClauseLock;

	// Structures for EXPORTING
	std::vector<Bucket*> buckets;
	std::vector<std::vector<int> > vipClauses;

	// Structures for IMPORTING	
	int* incomingBuffer;
	size_t bufferSize;
	size_t currentPos;
	size_t currentSize; // 0 for VIP clauses
	int remainingVipLits;
	int remainingClsOfCurrentSize;

};

#endif /* CLAUSEDATABASE_H_ */

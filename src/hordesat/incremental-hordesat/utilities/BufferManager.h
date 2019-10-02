/*
 * BufferManager.h
 *
 *  Created on: May 27, 2015
 *      Author: balyo
 */

#ifndef BUFFERMANAGER_H_
#define BUFFERMANAGER_H_

#include <map>
#include <vector>

using namespace std;

class BufferManager {
private:
	map<int, vector<int*> > returnedBuffers;

public:
	BufferManager();

	/**
	 * Get a buffer of a given size, return it after usage to avoid memory leak.
	 */
	int* getBuffer(int size);

	/**
	 * Return a buffer, it will be recycled or deallocated.
	 */
	void returnBuffer(int* location);

	/**
	 * Deallocate returned and unused buffers
	 */
	void cleanReturnedBuffers();
	virtual ~BufferManager();
};

#endif /* BUFFERMANAGER_H_ */

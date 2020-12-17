/*
 * BufferManager.cpp
 *
 *  Created on: May 27, 2015
 *      Author: balyo
 */

#include "buffer_manager.hpp"

BufferManager::BufferManager() {
}

int* BufferManager::getBuffer(int size) {
	std::vector<int*>& buffers = returnedBuffers[size];
	if (buffers.size() > 0) {
		int* buffer = buffers[buffers.size() - 1];
		buffers.pop_back();
		return buffer;
	}
	int* buffer = new int[size+1];
	buffer[0] = size;
	buffer++;
	return buffer;
}

void BufferManager::returnBuffer(int* location) {
	returnedBuffers[location[-1]].push_back(location);
}

void BufferManager::cleanReturnedBuffers() {
	for (std::map<int, std::vector<int*> >::iterator it = returnedBuffers.begin(); it != returnedBuffers.end(); ++it) {
		for (std::vector<int*>::iterator bit = it->second.begin(); bit != it->second.end(); ++bit) {
			delete[] (*bit - 1);
		}
		it->second.clear();
	}
	returnedBuffers.clear();
}


BufferManager::~BufferManager() {
	cleanReturnedBuffers();
}


#ifndef DEBUGUTILS_H_
#define DEBUGUTILS_H_

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <vector>

using namespace std;

//DEBUG MACROS
//#define DEBUG(X) X
#define DEBUG(X)

/**
 * Print array of a given length
 */
inline void printArray(const int* s, int len) {
	for (int i = 0; i < len; i++) {
		printf("%d ", s[i]);
	}
	printf("\n");
}

/**
 * Print array of length parts*size, each part on a line
 */
inline void printArray(const int* s, int parts, int size) {
	for (int part = 0; part < parts; part++) {
		printf("Part %d:", part);
		for (int j = 0; j < size; j++) {
			printf("%d ", s[j+(part*size)]);
		}
		printf("\n");
	}
}

/**
 * Print a clause (vector of integers)
 */
inline void printVector(const vector<int>& vec) {
	printf("[");
	if (vec.size() > 0) {
		printf("%d",vec[0]);
	}
	for (size_t i = 1; i < vec.size(); i++) {
		printf(", %d", vec[i]);
	}
	printf("]\n");
}

/**
 * Print zero-terminated array
 */
inline void printArrayZT(int * s, int sep = 0) {
	while(*s != sep) {
		printf("%d ", *s);
		s++;
	}
	printf("\n");
}

#endif /* DEBUGUTILS_H_ */

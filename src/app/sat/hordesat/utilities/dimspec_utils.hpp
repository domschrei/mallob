/*
 * DimspecUtils.h
 *
 *  Created on: Nov 20, 2017
 *      Author: balyo
 */

#ifndef DIMSPECUTILS_H_
#define DIMSPECUTILS_H_

#include <vector>

using namespace std;

struct CnfFormula {
	int variables;
	vector<vector<int> > clauses;
};

struct DimspecFormula {
	CnfFormula init, goal, universal, transition;
};

struct DimspecSolution {
	vector<vector<int> > values;
};

DimspecFormula readDimspecProblem(const char* filename);
bool checkDimspecSolution(const DimspecFormula& f, const DimspecSolution& s);
bool checkDimspecValidity(const DimspecFormula& f);
#endif /* DIMSPECUTILS_H_ */

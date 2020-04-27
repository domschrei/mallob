/*
 * satUtils.h
 *
 *  Created on: Dec 4, 2014
 *      Author: balyo
 */

#ifndef SATUTILS_H_
#define SATUTILS_H_

#include "../solvers/PortfolioSolverInterface.h"

bool loadFormulaToSolvers(vector<PortfolioSolverInterface*> solvers, const char* filename);

#endif /* SATUTILS_H_ */

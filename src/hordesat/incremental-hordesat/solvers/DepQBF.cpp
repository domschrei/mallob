/*
 * DepQBF.cpp
 *
 *  Modified by Florian Lonsing, January 2015
 *
 *  Created on: Nov 11, 2014
 *      Author: balyo
 */

#include "DepQBF.h"
#include <ctype.h>
#include <stdio.h>
#include "../utilities/DebugUtils.h"

extern "C" {
#include "../../depQBF/qdpll.h"
}

/* Inspired by Lingeling's implementation). Parameter 'size' is the number of
 literals in the clause/cube 'cls', EXCLUDING the marker literal. */
//NOTE: in DepQBF we call this function only when using QPUP-based learning
void qbfproduce(void *sp, int *cls, int size) {
	DepQBF* depqbf = (DepQBF*) sp;

	/* Ignore clauses/cubes longer than 'size'. */
	if (size > depqbf->sizeLimit) {
		return;
	}

	vector<int> vcls;
	int i = 0;
	/* Go from index 0 to 'size' since 'size' is the number of literals in 'cls'
	 EXCLUDING the marker literal. */
	while (i <= size) {
		vcls.push_back(cls[i]);
		i++;
	}

	depqbf->callback->processClause(vcls, depqbf->myId);
}

/* Inspired by Lingeling's implementation). Parameter 'param' is actually
 redundant. */
void qbfconsumeCls(void *sp, int **clause, int *param) {
	DepQBF* depqbf = (DepQBF*) sp;

	if (depqbf->learnedClausesToAdd.empty()) {
		*clause = NULL;
		return;
	}
	if (depqbf->clauseAddMutex.tryLock() == false) {
		*clause = NULL;
		return;
	}
	vector<int> cls = depqbf->learnedClausesToAdd.back();
	depqbf->learnedClausesToAdd.pop_back();

	if (cls.size() >= depqbf->clsBufferSize) {
		depqbf->clsBufferSize = 2 * cls.size();
		depqbf->clsBuffer = (int*) realloc((void*) depqbf->clsBuffer,
				depqbf->clsBufferSize * sizeof(int));
	}

	for (size_t i = 0; i < cls.size(); i++) {
		depqbf->clsBuffer[i] = cls[i];
	}

	depqbf->clsBuffer[cls.size()] = 0;
	*clause = depqbf->clsBuffer;
	depqbf->clauseAddMutex.unlock();
}

/* ================================================ */

DepQBF::DepQBF() {
	solver = qdpll_create();
	/* Configure DepQBF to use linear quantifier ordering. This way, sharing
	 learned clauses is simpler as we do not have to apply universal reduction
	 when adding shared learned clauses. */
	char * arg = (char *) "--dep-man=simple";
	qdpll_configure(solver, arg);
	//TODO: maybe set more options here, if necessary

	stopSolver = 0;
	qdpll_set_interrupt_flag(solver, &stopSolver);

	callback = NULL;
	/* Initially, share only unit clauses. */
	sizeLimit = 1;

	unitsBufferSize = clsBufferSize = 100;
	unitsBuffer = (int *) malloc(unitsBufferSize * sizeof(int));
	clsBuffer = (int *) malloc(clsBufferSize * sizeof(int));
	myId = 0;
}

/* Same as in Lingeling's implementation. */
bool DepQBF::loadFormula(const char *filename) {
	return qdpll_load_formula(solver, filename);
#if 0
	vector < PortfolioSolverInterface * >solvers;
	solvers.push_back (this);
	return loadFormulaToSolvers (solvers, filename);
#endif
}

/* Not yet implemented. */
int DepQBF::getVariablesCount() {
	return -1;
}

/* Not yet implemented. */
int DepQBF::getSplittingVariable() {
	return -1;
}

/* Not yet implemented. */
void DepQBF::setPhase(const int var, const bool phase) {

}

void DepQBF::setSolverInterrupt() {
	stopSolver = 1;
}

void DepQBF::unsetSolverInterrupt() {
	stopSolver = 0;
}

// Solve the formula with a given set of assumptions
// return 10 for SAT, 20 for UNSAT, 0 for UNKNOWN
SatResult DepQBF::solve(const vector<int>&assumptions) {
	// add the clauses
	clauseAddMutex.lock();
	for (size_t i = 0; i < clausesToAdd.size(); i++) {
		for (size_t j = 0; j < clausesToAdd[i].size(); j++) {
			qdpll_add(solver, clausesToAdd[i][j]);
		}
		qdpll_add(solver, 0);
	}
	clausesToAdd.clear();
	clauseAddMutex.unlock();

	// set the assumptions
	for (size_t i = 0; i < assumptions.size(); i++) {
		// freezing problems
		qdpll_assume(solver, assumptions[i]);
	}
	QDPLLResult res = qdpll_sat(solver);
	switch (res) {
	case QDPLL_RESULT_SAT:
		return SAT;
	case QDPLL_RESULT_UNSAT:
		return UNSAT;
	case QDPLL_RESULT_UNKNOWN:
		return UNKNOWN;
	}
	return UNKNOWN;
}

vector<int> DepQBF::getSolution() {
	throw logic_error("not implemented");
}

set<int> DepQBF::getFailedAssumptions() {
	throw logic_error("not implemented");
}

void DepQBF::addLiteral(int lit) {
	qdpll_add(solver, lit);
}

// Add a permanent clause to the formula
void DepQBF::addClause(vector<int>&clause) {
	clauseAddMutex.lock();
	clausesToAdd.push_back(clause);
	clauseAddMutex.unlock();
}

void DepQBF::addClauses(vector<vector<int> >& clauses) {
	clauseAddMutex.lock();
	clausesToAdd.insert(clausesToAdd.end(), clauses.begin(), clauses.end());
	clauseAddMutex.unlock();
}

void DepQBF::addInitialClauses(vector<vector<int> >& clauses) {
	for (size_t i = 0; i < clauses.size(); i++) {
		for (size_t j = 0; j < clauses[i].size(); j++) {
			int lit = clauses[i][j];
			qdpll_add(solver, lit);
		}
		qdpll_add(solver, 0);
	}
}

// Add a learned clause to the formula
void DepQBF::addLearnedClause(vector<int>&clause) {
	clauseAddMutex.lock();
	if (clause.size() == 1) {
		unitsToAdd.push_back(clause[0]);
	} else {
		learnedClausesToAdd.push_back(clause);
	}
	clauseAddMutex.unlock();
}

void DepQBF::addLearnedClauses(vector<vector<int> >& clauses) {
	clauseAddMutex.lock();
	for (size_t i = 0; i < clauses.size(); i++) {
		if (clauses[i].size() == 1) {
			unitsToAdd.push_back(clauses[i][0]);
		} else {
			learnedClausesToAdd.push_back(clauses[i]);
		}
	}
	clauseAddMutex.unlock();
}

/* Increase size of constraints to be shared. */
void DepQBF::increaseClauseProduction() {
	sizeLimit++;
}

void DepQBF::setLearnedClauseCallback(LearnedClauseCallback * callback,
		int solverId) {
	this->callback = callback;
	myId = solverId;
	qdpll_set_constraint_consume_callback(solver, qbfconsumeCls, this);
	qdpll_set_constraint_produce_callback(solver, qbfproduce, this);
}

/* TODO: TO BE IMPLEMENTED */
SolvingStatistics DepQBF::getStatistics() {
	SolvingStatistics st;
	return st;
}

void DepQBF::diversify(int rank, int size) {
	/* Configure random number seed via string parameter. */
	char opt_buffer[1024];
	sprintf(opt_buffer, "--seed=%d", rank);
	qdpll_configure(solver, opt_buffer);

	/* We make sure that the solver behavior is diversified by randomization in
	 every case. Additionally, we enable/disable certain techniques. */

	switch (rank % 8) {

	case 0:
	default:
		/* Enable long-distance resolution. */
		sprintf(opt_buffer, "--long-dist-res");
		qdpll_configure(solver, opt_buffer);
	case 1:
		/* Fall-through case to ensure random diversification also for the case
		 above.*/
		qdpll_randomize_assignment_cache(solver);
		break;

	case 2:
		/* Switch off QBCE at all. */
		sprintf(opt_buffer, "--no-qbce-dynamic");
		qdpll_configure(solver, opt_buffer);
	case 3:
		/* Fall-through case to ensure random diversification also for the case
		 above.*/
		qdpll_randomize_var_activity_scaling(solver);
		break;

	case 4:
		/* Switch on QBCE inprocessing. */
		sprintf(opt_buffer, "--no-qbce-dynamic");
		qdpll_configure(solver, opt_buffer);
		sprintf(opt_buffer, "--qbce-inprocessing");
		qdpll_configure(solver, opt_buffer);
	case 5:
		/* Fall-through case to ensure random diversification also for the case
		 above.*/
		qdpll_randomize_learned_constraint_removal(solver);
		break;

	case 6:
		/* Switch on QBCE inprocessing. */
		sprintf(opt_buffer, "--no-qbce-dynamic");
		qdpll_configure(solver, opt_buffer);
		sprintf(opt_buffer, "--qbce-preprocessing");
		qdpll_configure(solver, opt_buffer);
	case 7:
		/* Fall-through case to ensure random diversification also for the case
		 above.*/
		qdpll_randomize_restart_schedule(solver);
		break;

	}
}

DepQBF::~DepQBF() {
	qdpll_delete(solver);
	free(unitsBuffer);
	free(clsBuffer);
}

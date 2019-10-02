/*
 * DimspecUtils.cpp
 *
 *  Created on: Nov 20, 2017
 *      Author: balyo
 */

#include "DimspecUtils.h"
#include <stdio.h>
#include <ctype.h>
#include "Logger.h"
#include "DebugUtils.h"

bool checkDimspecValidity(const DimspecFormula& f) {
	if (f.init.variables != f.goal.variables || f.init.variables != f.universal.variables ||
			2*f.init.variables != f.transition.variables) {
		exitError("Variable numbers inconsistent in I,G,U,T = %d, %d, %d, %d\n",
				f.init.variables, f.goal.variables, f.universal.variables, f.transition.variables);
	}
	log(0, "Formula loaded, it has %d variables, number of clauses in I,G,U,T is %d, %d, %d, %d\n",
			f.init.variables, f.init.clauses.size(), f.goal.clauses.size(), f.universal.clauses.size(),
			f.transition.clauses.size());
	return true;
}


DimspecFormula readDimspecProblem(const char* filename) {
	FILE* f = fopen(filename, "r");
	if (f == NULL) {
		exitError("Failed to open input file (%s)\n", filename);
	}
	DimspecFormula fla;
	CnfFormula* cf = NULL;

	int vars, cls;
	char kar;
	int c = 0;
	bool neg = false;
	vector<int> clause;

	while (c != EOF) {
		c = fgetc(f);

		// problem definition line
		if (c == 'i' || c == 'g' || c == 'u' || c == 't') {
			char pline[512];
			int i = 0;
			while (c != '\n') {
				pline[i++] = c;
				c = fgetc(f);
			}
			pline[i] = 0;
			if (3 != sscanf(pline, "%c cnf %d %d", &kar, &vars, &cls)) {
				exitError("Failed to parse the problem definition line (%s)\n", pline);
			}
			switch(kar) {
			case 'i': cf = &fla.init;
			break;
			case 'u': cf = &fla.universal;
			break;
			case 'g': cf = &fla.goal;
			break;
			case 't': cf = &fla.transition;
			break;
			default:
				exitError("Invalid formula identifier (%s)\n", pline);
			}
			cf->variables = vars;
			continue;
		}
		// comment
		if (c == 'c') {
			// skip this line
			while(c != '\n') {
				c = fgetc(f);
			}
			continue;
		}
		// whitespace
		if (isspace(c)) {
			continue;
		}
		// negative
		if (c == '-') {
			neg = true;
			continue;
		}
		// number
		if (isdigit(c)) {
			int num = c - '0';
			c = fgetc(f);
			while (isdigit(c)) {
				num = num*10 + (c-'0');
				c = fgetc(f);
			}
			if (neg) {
				num *= -1;
			}
			neg = false;
			if (num == 0) {
				cf->clauses.push_back(clause);
				clause.clear();
			} else {
				clause.push_back(num);
			}
		}
	}
	fclose(f);
	return fla;
}

bool checkClauses(const vector<vector<int> >& clauses, const vector<int>& values) {
	for (size_t i = 0; i < clauses.size(); i++) {
		bool sat = false;
		for (size_t j = 0; j < clauses[i].size(); j++) {
			int lit = clauses[i][j];
			int var = lit > 0 ? lit : -lit;
			if (values[var] == lit) {
				sat = true;
				break;
			}
		}
		if (!sat) {
			return false;
		}
	}
	return true;
}

bool checkTransitionClauses(const vector<vector<int> >& clauses, const vector<int>& values1, const vector<int>& values2) {
	int variables = values1.size() - 1;
	for (size_t i = 0; i < clauses.size(); i++) {
		bool sat = false;
		for (size_t j = 0; j < clauses[i].size(); j++) {
			int lit = clauses[i][j];
			int var = abs(lit);
			if (var <= variables && lit == values1[var]) {
				sat = true;
				break;
			}
			if (var > variables) {
				int val = values2[var - variables];
				val = val > 0 ? val+variables : val - variables;
				if (val == lit) {
					sat = true;
					break;
				}
			}
		}
		if (!sat) {
			return false;
		}
	}
	return true;
}

bool checkDimspecSolution(const DimspecFormula& f, const DimspecSolution& s) {
	if (!checkClauses(f.init.clauses, s.values[0])) {
		exitError("Wrong solution: Initial conditions unsat\n");
	}
	if (!checkClauses(f.goal.clauses, s.values[s.values.size() - 1])) {
		exitError("Wrong solution: Goal conditions unsat\n");
	}
	for (size_t step = 0; step < s.values.size(); step++) {
		if (!checkClauses(f.universal.clauses, s.values[step])) {
			exitError("Wrong solution, universal clauses do not hold in step %d\n", step);
		}
	}
	for (size_t step = 0; step+1 < s.values.size(); step++) {
		if (!checkTransitionClauses(f.transition.clauses, s.values[step], s.values[step+1])) {
			exitError("Wrong solution, transitional clauses do not hold between steps %d and %d\n", step, step+1);
		}
	}
	return true;
}

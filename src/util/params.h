/*
 * ParameterProcessor.h
 *
 *  Created on: Dec 5, 2014
 *      Author: balyo
 */

#ifndef DOMPASCH_PARAMETERPROCESSOR_H_
#define DOMPASCH_PARAMETERPROCESSOR_H_

#include "string.h"
#include <map>
#include <string>
#include <iostream>
#include "stdlib.h"
using namespace std;

#define ROUNDING_BISECTION "bisec"
#define ROUNDING_PROBABILISTIC "prob"
#define ROUNDING_FLOOR "floor"

class Parameters {
private:
	map<string, string> _params;
	string _filename;
public:
	Parameters() = default;
	void init(int argc, char** argv);
	void printUsage() const;
	void setDefaults();
	string getFilename() const;
	void printParams() const;
	
	void setParam(const char* name);
	void setParam(const char* name, const char* value);

	bool isSet(const string& name) const;
	string getParam(const string& name, const string& defaultValue) const;
	string getParam(const string& name) const;
	int getIntParam(const string& name, int defaultValue) const;
	float getFloatParam(const string& name, float defaultValue) const;
	int getIntParam(const string& name) const;
	float getFloatParam(const string& name) const;
};

#endif /* PARAMETERPROCESSOR_H_ */

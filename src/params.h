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

class Parameters {
private:
	map<string, string> params;
	string filename;
public:
	Parameters() = default;
	void init(int argc, char** argv);
	string getFilename();
	void printParams();
	void setParam(const char* name);
	void setParam(const char* name, const char* value);
	bool isSet(const string& name);
	string getParam(const string& name, const string& defaultValue);
	string getParam(const string& name);
	int getIntParam(const string& name, int defaultValue);
	float getFloatParam(const string& name, float defaultValue);
};

#endif /* PARAMETERPROCESSOR_H_ */

/*
 * ParameterProcessor.h
 *
 *  Created on: Dec 5, 2014
 *      Author: balyo
 */

#ifndef DOMPASCH_PARAMETERPROCESSOR_H_
#define DOMPASCH_PARAMETERPROCESSOR_H_

#include <map>
#include <string>
#include <iostream>

#include "util/robin_hood.hpp"
#include "util/option.hpp"

class Parameters {

public:
	#include "optionslist.hpp"

public:
	Parameters() = default;
	Parameters(const Parameters& other);

	void init(int argc, char** argv);
	void expand();

	void printUsage() const;
	void printParams() const;
	
	char* const* asCArgs(const char* execName) const;
};

#endif /* PARAMETERPROCESSOR_H_ */

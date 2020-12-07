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

#define ROUNDING_BISECTION "bisec"
#define ROUNDING_PROBABILISTIC "prob"
#define ROUNDING_FLOOR "floor"

class Parameters {

private:
	robin_hood::unordered_map<std::string, std::string> _params;

public:
	Parameters() = default;

	void init(int argc, char** argv);
	void setDefaults();
	void expand();

	void printUsage() const;
	void printParams() const;
	
	void setParam(const char* name);
	void setParam(const char* name, const char* value);
	void setParam(const std::string& name, const std::string& value);

	bool isSet(const std::string& name) const;
	bool isNotNull(const std::string& name) const;
	
	std::string getParam(const std::string& name) const;
	std::string getParam(const std::string& name, const std::string& defaultValue) const;
	const std::string& operator[](const std::string& key) const;
	std::string& operator[](const std::string& key);

	int getIntParam(const std::string& name) const;
	int getIntParam(const std::string& name, int defaultValue) const;
	
	float getFloatParam(const std::string& name) const;
	float getFloatParam(const std::string& name, float defaultValue) const;

	double getDoubleParam(const std::string& name) const;
	double getDoubleParam(const std::string& name, double defaultValue) const;

	char* const* asCArgs(const char* execName) const;
};

#endif /* PARAMETERPROCESSOR_H_ */

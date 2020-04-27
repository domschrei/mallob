/*
 * ParameterProcessor.h
 *
 *  Created on: Dec 5, 2014
 *      Author: balyo
 */

#ifndef PARAMETERPROCESSOR_H_
#define PARAMETERPROCESSOR_H_

#include "string.h"
#include "stdlib.h"

#include <map>
#include <string>
#include <iostream>
#include <memory>
#include <assert.h>

#include "logging_interface.h"

using namespace std;


class ParameterProcessor {
private:
	map<string, string> params;
	char* filename;
	std::shared_ptr<LoggingInterface> logger = NULL;

public:
	ParameterProcessor() {
		filename = NULL;
	}
	void init(int argc, char** argv) {
		for (int i = 1; i < argc; i++) {
			char* arg = argv[i];
			if (arg[0] != '-') {
				filename = arg;
				continue;
			}
			char* eq = strchr(arg, '=');
			if (eq == NULL) {
				params[arg+1];
			} else {
				*eq = 0;
				char* left = arg+1;
				char* right = eq+1;
				params[left] = right;
			}
		}
	}

	void setLogger(std::shared_ptr<LoggingInterface>& logger) {
		this->logger = logger;
	}
	LoggingInterface& getLogger() {
		assert(logger != NULL);
		return *logger;
	}

	const char* getFilename() {
		return filename;
	}

	void printParams() {
		std::string out = "";
		for (map<string,string>::iterator it = params.begin(); it != params.end(); it++) {
			if (it->second.empty()) {
				out.append(it->first + " ");
			} else {
				out.append(it->first + "=" + it->second + " ");
			}
		}
		assert(logger != NULL);
		logger->log(0, out.c_str());
	}

	void setParam(const char* name) {
		params[name];
	}

	void setParam(const char* name, const char* value) {
		params[name] = value;
	}

	bool isSet(const string& name) {
		return params.find(name) != params.end();
	}

	string getParam(const string& name, const string& defaultValue) {
		if (isSet(name)) {
			return params[name];
		} else {
			return defaultValue;
		}
	}

	string getParam(const string& name) {
		return getParam(name, "ndef");
	}

	int getIntParam(const string& name, int defaultValue) {
		if (isSet(name)) {
			return atoi(params[name].c_str());
		} else {
			return defaultValue;
		}
	}

};

#endif /* PARAMETERPROCESSOR_H_ */

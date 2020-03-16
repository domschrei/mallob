/*
 * ParameterProcessor.h
 *
 *  Created on: Dec 5, 2014
 *      Author: balyo
 */

#ifndef PARAMETERPROCESSOR_H_
#define PARAMETERPROCESSOR_H_

#include "string.h"
#include <map>
#include <string>
#include <iostream>
#include "stdlib.h"
using namespace std;


class ParameterProcessor {
private:
	map<string, string> params;
	char* filename;
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

	const char* getFilename() {
		return filename;
	}

	void printParams() {
		for (map<string,string>::iterator it = params.begin(); it != params.end(); it++) {
			if (it->second.empty()) {
				cout << it->first << ", ";
			} else {
				cout << it->first << "=" << it->second << ", ";
			}
		}
		cout << "\n";
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

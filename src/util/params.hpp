
#ifndef DOMPASCH_PARAMETERPROCESSOR_H_
#define DOMPASCH_PARAMETERPROCESSOR_H_

#include <list>

#include "util/option.hpp"

class Parameters {

public:
	#include "optionslist.hpp"

public:
	Parameters() = default;
	Parameters(const Parameters& other);

	void init(int argc, char** argv);
	void expand();

	void printBanner() const;
	void printUsage() const;
	std::string getParamsAsString() const;
	
	std::string getSubprocCommandAsString(const char* execName);
	std::list<std::string>& getArgList(const char* execName);
	char* const* asCArgs(const char* execName);

private:
	std::list<std::string> _list_for_c_args;

};

#endif
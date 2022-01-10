
#ifndef DOMPASCH_PARAMETERPROCESSOR_H_
#define DOMPASCH_PARAMETERPROCESSOR_H_

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
	void printParams() const;
	
	char* const* asCArgs(const char* execName) const;
};

#endif
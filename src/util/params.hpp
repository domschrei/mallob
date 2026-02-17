
#ifndef DOMPASCH_PARAMETERPROCESSOR_H_
#define DOMPASCH_PARAMETERPROCESSOR_H_

#include <list>
#include <string>

#include "util/option.hpp"

class Parameters {

public:
	#include "optionslist.hpp"

public:
	Parameters() = default;
	Parameters(const Parameters& other);

	void init(int argc, char** argv);
	void init(const std::vector<std::string>& args);
	void expand();

	void printBanner() const;
	void printUsage() const;
	std::string getParamsAsString() const;
	std::vector<std::string> getParamsAsStringList() const;
	
	std::string getSubprocCommandAsString(const char* execName, bool appendOptions) const;
	std::list<std::string>& getArgList(const char* execName);
	char* const* asCArgs(const char* execName);

private:
	std::list<std::string> _list_for_c_args;
	void process(const char* arg, const robin_hood::unordered_node_map<std::string, std::string>& longToShortOpt);

};

#endif
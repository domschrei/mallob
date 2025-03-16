
#include <sstream>
#include <string.h>
#include <assert.h>
#include <map>
#include <istream>
#include <iterator>
#include <algorithm>
#include <type_traits>
#include <utility>
#include <vector>

#include "params.hpp"
#include "logger.hpp"
#include "comm/sysstate.hpp"
#include "util/option.hpp"
#include "util/robin_hood.hpp"

const char* BANNER = "\nMallob -- a parallel and distributed platform for job scheduling, load balancing, and SAT solving\nDesigned by P. Sanders and D. Schreiber 2018-2022\nDeveloped by D. Schreiber 2019-2022\n";
const char* BANNER_C_PREFIXED = "c \nc Mallob -- a parallel and distributed platform for job scheduling, load balancing, and SAT solving\nc Designed by P. Sanders and D. Schreiber 2018-2022\nc Developed by D. Schreiber 2019-2022\nc ";
const char* USAGE = "Usage: [mpiexec -np <num-mpi-processes> [mpi-options]] mallob [options]\n";

Parameters::Parameters(const Parameters& other) {
    for (const auto& [id, opt] : other._global_map) {
        _global_map.at(id)->copyValue(*opt);
    }
}

/**
 * Taken partially from Hordesat:ParameterProcessor.h by Tomas Balyo.
 */
void Parameters::init(int argc, char** argv) {

    // Create dictionary mapping long option names to short option names
    robin_hood::unordered_node_map<std::string, std::string> longToShortOpt;
    for (const auto& [id, opt] : _global_map) {
        if (opt->hasLongOption()) {
            longToShortOpt[opt->longid] = opt->id;
        }
    }

    // Iterate over all arguments
    for (int i = 1; i < argc; i++) {
        char* arg = argv[i];
        
        // first option dash
        if (arg[0] != '-') {
            LOG(V1_WARN, "[WARN] Invalid argument \"%s\"\n", arg);
            continue;
        }
        arg = arg+1;
        // optional second option dash
        if (arg[0] == '-') arg = arg+1;

        char* eq = strchr(arg, '=');
        if (eq == NULL) {
            // No equals sign in this argument: set arg to 1 implicitly
            const char* left = arg;
            if (longToShortOpt.count(left)) left = longToShortOpt[left].c_str();
            if (_global_map.count(left)) {
                _global_map.at(left)->setValAsString("1");
            }
        } else {
            // Divide string at equals sign, set arg to given value
            *eq = 0;
            const char* left = arg;
            const char* right = eq+1;
            if (longToShortOpt.count(left)) left = longToShortOpt[left].c_str();
            if (_global_map.count(left)) {
                _global_map.at(left)->setValAsString(right);
            }
        }
    }

    // Expand and propagate options as necessary
    expand();

}

void Parameters::expand() {
    if (monoFilename.isSet()) {
        // Single instance solving
        numClients.set(1); // 1 client
        useFilesystemInterface.set(false); // no fs interface
        useIPCSocketInterface.set(false); // no socket interface
        numWorkers.set(-1); // all workers
        loadFactor.set(1); // full load factor
        maxDemand.set(0); // no limit of max. demand
        jobCacheSize.set(1); // only remember a single job desc. at a time
    }
}

void Parameters::printBanner() const {
    // Output program banner (only the PE of rank zero)
    LOG_OMIT_PREFIX(V2_INFO, "%s\n", monoFilename.isSet() ? BANNER_C_PREFIXED : BANNER);
}

void Parameters::printUsage() const {
    LOG(V2_INFO, USAGE);
    LOG(V2_INFO, "Each option must be given as \"-key=value\" or \"--key=value\" or (for boolean options only) just \"-[-]key\".\n");
    
    auto& groups = _grouped_list;
    for (auto& group : groups) {
 
        // Output group header, formatted based on num. levels in the group's ID
        int numLevels = 0;
        for (auto c : group->groupId) if (c == '/') numLevels++;
        const char* secondLinebreak = group->map.empty() ? "" : "\n";
        if (numLevels == 0) LOG_OMIT_PREFIX(V2_INFO, "\n=== %s ===%s\n", group->desc.c_str(), secondLinebreak);
        else if (numLevels == 1) LOG_OMIT_PREFIX(V2_INFO, "\n* %s *%s\n", group->desc.c_str(), secondLinebreak);
        else LOG_OMIT_PREFIX(V2_INFO, "\n%s:\n", group->desc.c_str());

        auto& map = group->map;

        std::map<std::string, Option*> sortedMap;
        for (const auto& [id, opt] : map) {
            sortedMap[id] = opt;
        }

        for (const auto& [id, opt] : sortedMap) {
            std::string defaultVal = opt->getValAsString();
            if (!defaultVal.empty()) defaultVal = ", default: " + defaultVal;
            
            const char* typeStr = opt->getTypeString();

            if (opt->hasLongOption()) {
                LOG_OMIT_PREFIX(V2_INFO, "-%s , -%s (%s%s)\n\t%s\n", 
                    id.c_str(), opt->longid.c_str(), typeStr, defaultVal.c_str(), opt->desc.c_str());
            } else {
                LOG_OMIT_PREFIX(V2_INFO, "-%s (%s%s)\n\t%s\n", 
                    id.c_str(), typeStr, defaultVal.c_str(), opt->desc.c_str());
            }
        }
    }
}

std::string Parameters::getParamsAsString() const {
    std::string out = "";
    std::map<std::string, std::string> sortedParams;
    for (const auto& [id, opt] : _global_map) {
        sortedParams[id] = opt->getValAsString();
    }
    for (const auto& it : sortedParams) {
        if (!it.second.empty()) {
            out += "-" + it.first + "=" + it.second + " ";
        }
    }
    return out;
}

std::string Parameters::getSubprocCommandAsString(const char* execName) const {

    std::string out;

    // Subprocess prefix (can be multiple words)
    if (subprocessPrefix.isSet()) {
        std::istringstream buffer(subprocessPrefix()); 
        std::vector<std::string> words = std::vector<std::string>{std::istream_iterator<std::string>(buffer), {}};
        for (auto& word : words) out += word + " ";
    }
    // Executable name
    out += execName + std::string(" ");
    // Options
    for (const auto& [id, opt] : _global_map) if (!opt->getValAsString().empty()) {
        out += ("-" + id + "=" + opt->getValAsString()) + " ";
    }

    return out.substr(0, out.size()-1);
}

std::list<std::string>& Parameters::getArgList(const char* execName) {
    
    _list_for_c_args.clear();

    // Subprocess prefix (can be multiple words)
    if (subprocessPrefix.isSet()) {
        std::istringstream buffer(subprocessPrefix()); 
        std::vector<std::string> words = std::vector<std::string>{std::istream_iterator<std::string>(buffer), {}};
        for (auto& word : words) _list_for_c_args.push_back(word);
    }
    // Executable name
    _list_for_c_args.push_back(execName);
    // Options
    for (const auto& [id, opt] : _global_map) if (!opt->getValAsString().empty()) {
        _list_for_c_args.push_back("-" + id + "=" + opt->getValAsString());
    }

    return _list_for_c_args;
}

char* const* Parameters::asCArgs(const char* execName) {

    auto& list = getArgList(execName);
    const char** argv = new const char*[list.size()+1];
    size_t i = 0;
    for (auto it = list.begin(); it != list.end(); ++it) {
        argv[i++] = it->c_str();
    }
    argv[i++] = nullptr;
    assert(i == list.size()+1);
    return (char* const*) argv;
}
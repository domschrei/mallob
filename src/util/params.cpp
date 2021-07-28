
#include <assert.h>
#include <string.h>
#include <map>

#include "params.hpp"
#include "logger.hpp"

const char* USAGE = "Usage: [mpiexec -np <num-mpi-processes> [mpi-options]] mallob [options]\n";

Parameters::Parameters(const Parameters& other) {
    for (const auto& [id, opt] : other._map) {
        _map.at(id)->copyValue(*opt);
    }
}

/**
 * Taken from Hordesat:ParameterProcessor.h by Tomas Balyo.
 */
void Parameters::init(int argc, char** argv) {

    // Create dictionary mapping long option names to short option names
    robin_hood::unordered_node_map<std::string, std::string> longToShortOpt;
    for (const auto& [id, opt] : _map) {
        if (opt->hasLongOption()) {
            longToShortOpt[opt->longid] = opt->id;
        }
    }

    for (int i = 1; i < argc; i++) {
        char* arg = argv[i];
        if (arg[0] != '-') {
            continue;
        }
        char* eq = strchr(arg, '=');
        if (eq == NULL) {
            const char* left = arg+1;
            if (longToShortOpt.count(left)) left = longToShortOpt[left].c_str();
            if (_map.count(left)) {
                _map.at(left)->setValAsString("1");
            }
        } else {
            *eq = 0;
            const char* left = arg+1;
            const char* right = eq+1;
            if (longToShortOpt.count(left)) left = longToShortOpt[left].c_str();
            if (_map.count(left)) {
                _map.at(left)->setValAsString(right);
            }
        }
    }
    expand();
}

void Parameters::expand() {
    if (monoFilename.isSet()) {
        // Single instance solving
        hopsUntilCollectiveAssignment.set(-1); // no collective assignments
        numClients.set(0); // no clients
        collectClauseHistory.set(false); // no clause history
        growthPeriod.set(0); // instantaneous growth of job demand
        loadFactor.set(1); // full load factor
        maxDemand.set(0); // no limit of max. demand
        balancingPeriod.set(0.01); // low balancing delay to immediately get full demand
    }
}

void Parameters::printUsage() const {
    log(V2_INFO, USAGE);
    
    std::map<std::string, Option*> sortedMap;
    for (const auto& [id, opt] : _map) {
        sortedMap[id] = opt;
    }

    for (const auto& [id, opt] : sortedMap) {
        std::string defaultVal = opt->getValAsString();
        if (!defaultVal.empty()) defaultVal = ", default: " + defaultVal;
        
        const char* typeStr = opt->getTypeString();

        if (opt->hasLongOption()) {
            log(LOG_NO_PREFIX | V2_INFO, "-%s , -%s (%s%s)\n\t\t%s\n", 
                id.c_str(), opt->longid.c_str(), typeStr, defaultVal.c_str(), opt->desc.c_str());
        } else {
            log(LOG_NO_PREFIX | V2_INFO, "-%s (%s%s)\n\t\t%s\n", 
                id.c_str(), typeStr, defaultVal.c_str(), opt->desc.c_str());
        }
    }
}

void Parameters::printParams() const {
    std::string out = "";
    std::map<std::string, std::string> sortedParams;
    for (const auto& [id, opt] : _map) {
        sortedParams[id] = opt->getValAsString();
    }
    for (const auto& it : sortedParams) {
        if (!it.second.empty()) {
            out += "-" + it.first + "=" + it.second + " ";
        }
    }
    log(V2_INFO, "Program options: %s\n", out.c_str());
}

char* const* Parameters::asCArgs(const char* execName) const {

    size_t numArgs = 0;
    for (const auto& [id, opt] : _map) if (!opt->getValAsString().empty()) numArgs++;

    const char** argv = new const char*[numArgs+2];
    argv[0] = execName;
    int i = 1;
    for (const auto& [id, opt] : _map) {
        std::string val = opt->getValAsString();
        if (val.empty()) continue;
        size_t argsize = 1 + id.size() + (!val.empty() ? 1 + val.size() : 0) + 1;
        char* arg = (char*) malloc(argsize * sizeof(char));
        arg[0] = '-';
        strncpy(arg+1, id.c_str(), id.size());
        if (!val.empty()) {
            arg[1+id.size()] = '=';
            strncpy(arg+(1+id.size()+1), val.c_str(), val.size());
        }
        arg[argsize-1] = '\0';

        argv[i] = arg;
        i++;
    }
    argv[i] = nullptr;
    return (char* const*) argv;
}
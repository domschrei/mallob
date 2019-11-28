
#include "assert.h"

#include "params.h"
#include "console.h"

/**
 * Taken from Hordesat:ParameterProcessor.h by Tomas Balyo.
 */
void Parameters::init(int argc, char** argv) {
    setDefaults();
    for (int i = 1; i < argc; i++) {
        char* arg = argv[i];
        if (arg[0] != '-') {
            filename = std::string(arg);
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

void Parameters::setDefaults() {
    setParam("c", "1"); // num clients
    //setParam("colors"); // colored terminal output
    //setParam("derandomize"); // derandomize job bouncing
    //setParam("h"); setParam("help"); // print usage
    setParam("g", "5.0"); // job demand growth interval
    setParam("l", "0.95"); // load factor
    setParam("log", "."); // logging directory
    setParam("lbc", "0"); // leaky bucket client parameter (0 = no leaky bucket, jobs enter by time) 
    setParam("p", "5.0"); // rebalance period (seconds)
    //setParam("q"); // no logging to stdout
    setParam("s", "1.0"); // job communication period (seconds)
    setParam("T", "0"); // total time to run the system (0 = no limit)
    setParam("t", "2"); // num threads per node
    setParam("td", "0.01"); // temperature decay for thermodyn. balancing
    setParam("cpuh-per-instance", "0"); // time limit per instance, in cpu hours (0 = no limit)
    setParam("time-per-instance", "0"); // time limit per instance, in seconds wall clock time (0 = no limit)
    setParam("v", "2"); // verbosity 0=CRIT 1=WARN 2=INFO 3=VERB 4=VVERB ...
    //setParam("warmup"); // warmup run
}

void Parameters::printUsage() {

    Console::log(Console::INFO, "Usage: mallob [options] <scenario>");
    Console::log(Console::INFO, "<scenario> : File path and name prefix for client scenario(s);");
    Console::log(Console::INFO, "             will parse <name>.0 for one client, ");
    Console::log(Console::INFO, "             <name>.0 and <name>.1 for two clients, ...");
    Console::log(Console::INFO, "Options:");
    Console::log(Console::INFO, "-c=<num-clients>      Amount of client nodes (int c >= 1)");
    Console::log(Console::INFO, "-colors               Colored terminal output based on messages' verbosity");
    Console::log(Console::INFO, "-derandomize          Derandomize job bouncing");
    Console::log(Console::INFO, "-g=<growth-period>    Grow job demand exponentially every t seconds (t >= 0; 0: immediate growth)");
    Console::log(Console::INFO, "-h|-help              Print usage");
    Console::log(Console::INFO, "-l=<load-factor>      Load factor to be aimed at (0 < l < 1)");
    Console::log(Console::INFO, "-lbc=<num-jobs>       Make each client a leaky bucket with x active jobs at any given time (int x >= 0, 0: jobs arrive at individual times instead)");
    Console::log(Console::INFO, "-log=<log-dir>        Directory to save logs in (default: .)");
    Console::log(Console::INFO, "-p=<rebalance-period> Do global rebalancing every t seconds (t > 0)");
    Console::log(Console::INFO, "-q                    Be quiet, do not log to stdout besides critical information");
    Console::log(Console::INFO, "-s=<comm-period>      Do job-internal communication every t seconds (t >= 0, 0: do not communicate)");
    Console::log(Console::INFO, "-T=<time-limit>       Run entire system for x seconds (x >= 0; 0: run indefinitely)");
    Console::log(Console::INFO, "-t=<num-threads>      Amount of worker threads per node (int t >= 1)");
    Console::log(Console::INFO, "-cpuh-per-instance=<time-limit> Timeout an instance after x cpu hours (x >= 0; 0: no timeout");
    Console::log(Console::INFO, "-time-per-instance=<time-limit> Timeout an instance after x seconds wall clock time (x >= 0; 0: no timeout");
    Console::log(Console::INFO, "-v=<verb-num>         Logging verbosity: 0=CRIT 1=WARN 2=INFO 3=VERB 4=VVERB ...");
    Console::log(Console::INFO, "-warmup               Do one explicit All-To-All warmup among all nodes in the beginning");
}

string Parameters::getFilename() {
  return filename;
}

void Parameters::printParams() {
    std::string out = "";
    for (map<string,string>::iterator it = params.begin(); it != params.end(); it++) {
        if (it->second.empty()) {
            out += it->first + ", ";
        } else {
            out += it->first + "=" + it->second + ", ";
        }
    }
    out = out.substr(0, out.size()-2);
    Console::log(Console::INFO, "Called with parameters: %s", out.c_str());
}

void Parameters::setParam(const char* name) {
    params[name];
}

void Parameters::setParam(const char* name, const char* value) {
    params[name] = value;
}

bool Parameters::isSet(const string& name) {
    return params.find(name) != params.end();
}

string Parameters::getParam(const string& name, const string& defaultValue) {
    if (isSet(name)) {
        return params[name];
    } else {
        return defaultValue;
    }
}

string Parameters::getParam(const string& name) {
    return getParam(name, "ndef");
}

int Parameters::getIntParam(const string& name, int defaultValue) {
    if (isSet(name)) {
        return atoi(params[name].c_str());
    } else {
        return defaultValue;
    }
}

int Parameters::getIntParam(const string& name) {
    assert(isSet(name));
    return atoi(params[name].c_str());
}

float Parameters::getFloatParam(const string& name, float defaultValue) {
    if (isSet(name)) {
        return atof(params[name].c_str());
    } else {
        return defaultValue;
    }
}

float Parameters::getFloatParam(const string& name) {
    assert(isSet(name));
    return atof(params[name].c_str());
}

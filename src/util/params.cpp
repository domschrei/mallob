
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
            _filename = std::string(arg);
            continue;
        }
        char* eq = strchr(arg, '=');
        if (eq == NULL) {
            _params[arg+1];
        } else {
            *eq = 0;
            char* left = arg+1;
            char* right = eq+1;
            _params[left] = right;
        }
    }
}

void Parameters::setDefaults() {
    setParam("ba", "4"); // num bounce alternatives (only relevant if -derandomize)
    setParam("bm", "ed"); // event-driven balancing (ed = event-driven, fp = fixed-period)
    setParam("c", "1"); // num clients
    setParam("cbbs", "1500"); // clause buffer base size
    setParam("cbdf", "1.0"); // clause buffer discount factor
    //setParam("cg"); // continuous growth
    //setParam("colors"); // colored terminal output
    //setParam("derandomize"); // derandomize job bouncing
    setParam("g", "5.0"); // job demand growth interval
    //setParam("h"); setParam("help"); // print usage
    setParam("jc", "0"); // job cache
    //setParam("jjp"); // jitter job priorities
    setParam("l", "0.95"); // load factor
    setParam("log", "."); // logging directory
    setParam("lbc", "0"); // leaky bucket client parameter (0 = no leaky bucket, jobs enter by time) 
    setParam("mcl", "0"); // maximum clause length (0 = no limit)
    setParam("md", "0"); // maximum demand per job (0 = no limit)
    //setParam("mmpi"); // monitor MPI
    //setParam("nophase"); // Do not do phase-based diversification (native only)
    setParam("p", "5.0"); // rebalance period (seconds)
    //setParam("q"); // no logging to stdout
    setParam("r", ROUNDING_BISECTION); // rounding of assignments (prob = probabilistic, bisec = iterative bisection)
    setParam("s", "1.0"); // job communication period (seconds)
    setParam("s2f", ""); // write solutions to file (file path, or empty string for no writing)
    setParam("T", "0"); // total time to run the system (0 = no limit)
    setParam("t", "2"); // num threads per node
    setParam("td", "0.01"); // temperature decay for thermodyn. balancing
    setParam("cpuh-per-instance", "0"); // time limit per instance, in cpu hours (0 = no limit)
    setParam("time-per-instance", "0"); // time limit per instance, in seconds wall clock time (0 = no limit)
    setParam("v", "2"); // verbosity 0=CRIT 1=WARN 2=INFO 3=VERB 4=VVERB ...
    //setParam("warmup"); // warmup run
    //setParam("yield"); // yield manager thread when no new messages
}

void Parameters::printUsage() const {

    Console::log(Console::INFO, "Usage: mallob [options] <scenario>");
    Console::log(Console::INFO, "<scenario> : File path and name prefix for client scenario(s);");
    Console::log(Console::INFO, "             will parse <name>.0 for one client, ");
    Console::log(Console::INFO, "             <name>.0 and <name>.1 for two clients, ...");
    Console::log(Console::INFO, "Options:");
    Console::log(Console::INFO, "-ba=<num-ba>          Number of bounce alternatives per node (only relevant if -derandomize)");
    Console::log(Console::INFO, "-bm=<balance-mode>    Balancing mode:");
    Console::log(Console::INFO, "                      \"fp\" - fixed-period");
    Console::log(Console::INFO, "                      \"ed\" (default) - event-driven");
    Console::log(Console::INFO, "-c=<num-clients>      Amount of client nodes (int c >= 1)");
    Console::log(Console::INFO, "-cbbs=<size>          Clause buffer base size in integers (default: 1500)");
    Console::log(Console::INFO, "-cbdf=<factor>        Clause buffer discount factor: reduce buffer size per node by <factor> each depth");
    Console::log(Console::INFO, "                      (0 < factor <= 1.0; default: 1.0)");
    Console::log(Console::INFO, "-cg                   Continuous growth of job demands: make job demands increase more finely grained"); 
    Console::log(Console::INFO, "                      (node by node instead of layer by layer)");
    Console::log(Console::INFO, "-colors               Colored terminal output based on messages' verbosity");
    Console::log(Console::INFO, "-cpuh-per-instance=<time-limit> Timeout an instance after x cpu hours (x >= 0; 0: no timeout)");
    Console::log(Console::INFO, "-derandomize          Derandomize job bouncing");
    Console::log(Console::INFO, "-g=<growth-period>    Grow job demand exponentially every t seconds (t >= 0; 0: immediate growth)");
    Console::log(Console::INFO, "-h|-help              Print usage");
    Console::log(Console::INFO, "-jc=<size>            Size of job cache for suspended, yet unfinished jobs (int x >= 0; 0: no limit)");
    Console::log(Console::INFO, "-jjp                  Jitter job priorities to break ties during rebalancing");
    Console::log(Console::INFO, "-l=<load-factor>      Load factor to be aimed at (0 < l < 1)");
    Console::log(Console::INFO, "-lbc=<num-jobs>       Make each client a leaky bucket with x active jobs at any given time");
    Console::log(Console::INFO, "                      (int x >= 0, 0: jobs arrive at individual times instead)");
    Console::log(Console::INFO, "-log=<log-dir>        Directory to save logs in (default: .)");
    Console::log(Console::INFO, "-mcl=<max-length>     Maximum clause length: Only share clauses up to some length (int x >= 0; 0: no limit)");
    Console::log(Console::INFO, "-md=<max-demand>      Limit any job's demand to some maximum value (int x >= 0; 0: no limit)");
    Console::log(Console::INFO, "-mmpi                 Monitor MPI: Launch an additional thread per process checking when the main thread");
    Console::log(Console::INFO, "                      is inside some MPI call");
    Console::log(Console::INFO, "-nophase              Do not diversify solvers based on phase; native diversification only");
    Console::log(Console::INFO, "-p=<rebalance-period> Do balancing every t seconds (t > 0). With -bm=ed : minimum delay between balancings");
    Console::log(Console::INFO, "-q                    Be quiet, do not log to stdout besides critical information");
    Console::log(Console::INFO, "-r=<round-mode>       Mode of rounding of assignments in balancing:");
    Console::log(Console::INFO, "                      \"prob\" - simple probabilistic rounding");
    Console::log(Console::INFO, "                      \"bisec\" (default) - iterative bisection to find optimal cutoff point");
    Console::log(Console::INFO, "                      \"floor\" - always round down");
    Console::log(Console::INFO, "-s=<comm-period>      Do job-internal communication every t seconds (t >= 0, 0: do not communicate)");
    Console::log(Console::INFO, "-s2f=<file-basename>  Write solutions to file with provided base name + job ID");
    Console::log(Console::INFO, "-sleep                Sleep in between polls of new messages");
    Console::log(Console::INFO, "-T=<time-limit>       Run entire system for x seconds (x >= 0; 0: run indefinitely)");
    Console::log(Console::INFO, "-t=<num-threads>      Amount of worker threads per node (int t >= 1)");
    Console::log(Console::INFO, "-time-per-instance=<time-limit> Timeout an instance after x seconds wall clock time (x >= 0; 0: no timeout)");
    Console::log(Console::INFO, "-v=<verb-num>         Logging verbosity: 0=CRIT 1=WARN 2=INFO 3=VERB 4=VVERB ...");
    Console::log(Console::INFO, "-warmup               Do one explicit All-To-All warmup among all nodes in the beginning");
    Console::log(Console::INFO, "-yield                Yield manager thread whenever there are no new messages");
}

string Parameters::getFilename() const {
  return _filename;
}

void Parameters::printParams() const {
    std::string out = "";
    for (const auto& it : _params) {
        if (it.second.empty()) {
            out += it.first + ", ";
        } else {
            out += it.first + "=" + it.second + ", ";
        }
    }
    out = out.substr(0, out.size()-2);
    Console::log(Console::INFO, "Called with parameters: %s", out.c_str());
}

void Parameters::setParam(const char* name) {
    _params[name];
}

void Parameters::setParam(const char* name, const char* value) {
    _params[name] = value;
}

bool Parameters::isSet(const string& name) const {
    return _params.find(name) != _params.end();
}

string Parameters::getParam(const string& name, const string& defaultValue) const {
    if (isSet(name)) {
        return _params.at(name);
    } else {
        return defaultValue;
    }
}

string Parameters::getParam(const string& name) const {
    return getParam(name, "ndef");
}

int Parameters::getIntParam(const string& name, int defaultValue) const {
    if (isSet(name)) {
        return atoi(_params.at(name).c_str());
    } else {
        return defaultValue;
    }
}

int Parameters::getIntParam(const string& name) const {
    assert(isSet(name));
    return atoi(_params.at(name).c_str());
}

float Parameters::getFloatParam(const string& name, float defaultValue) const {
    if (isSet(name)) {
        return atof(_params.at(name).c_str());
    } else {
        return defaultValue;
    }
}

float Parameters::getFloatParam(const string& name) const {
    assert(isSet(name));
    return atof(_params.at(name).c_str());
}


#include "assert.h"
#include "string.h"

#include "params.hpp"
#include "logger.hpp"

const char* OPTIONS = 
    "Usage: [mpiexec -np <num-mpi-processes> [mpi-options]] mallob [options]"

    "\n\nModes of operation:"
    "\nBy default, the JSON API to dynamically introduce jobs is enabled."
    "\nTo resolve a single SAT instance, use -mono."
    "\nTo test mallob under a fixed scenario of jobs to be processed, use -scenario."
    "\n-c=<num-clients>      Amount of client nodes (int c >= 1, or 0 iff -mono is set)"
    "\n-h|-help              Print usage and set parameters, then quit"
    "\n-lbc=<num-jobs>       Make each client a leaky bucket with x active jobs at any given time"
    "\n                      (int x >= 0, 0: jobs arrive at individual times instead)"
    "\n-mono=<filename>      Mono instance: Solve the provided CNF instance with full power, then exit."
    "\n                      NOTE: Overrides options; see mallob -mono=<filename> -h"
    "\n-scenario=<prefix>    Do not use JSON API but instead read jobs from static scenario files"
    "\n                      <prefix>.0 , ... , <prefix>.<#clients-1>"

    "\n\nSystem options:"
    "\n-appmode=<mode>       Application mode: \"fork\" (spawn child process for each job on each MPI process)"
    "\n                      or \"thread\" (execute jobs in separate threads but within the same process)"
    "\n-delaymonkey[=<0|1>]  Small chance for each MPI call to block for some random amount of time"
    "\n-jc=<size>            Size of job cache for suspended yet unfinished jobs (int x >= 0; 0: no limit)"
    "\n-latencymonkey[=<0|1>]    Block all MPI_Isend operations by a small randomized amount of time"
    "\n-mmpi[=<0|1>]         Monitor MPI: Launch an additional thread per process checking when the main thread"
    "\n                      is inside some MPI call"
    "\n-sleep=<micros>       Sleep provided number of microseconds between loop cycles of worker main thread"
    "\n-T=<time-limit>       Run entire system for x seconds (x >= 0; 0: run indefinitely)"
    "\n-t=<num-threads>      Amount of worker threads per node (int t >= 1)"
    "\n-warmup[=<0|1>]       Do one explicit All-To-All warmup among all nodes in the beginning"
    "\n-yield[=<0|1>]        Yield manager thread whenever there are no new messages"

    "\n\nOutput options:"
    "\n-colors[=<0|1>]       Colored terminal output based on messages' verbosity"
    "\n-log=<log-dir>        Directory to save logs in"
    "\n-q[=<0|1>]            Quiet mode: do not log to stdout besides critical information"
    "\n-s2f=<file-basename>  Write solutions to file with provided base name + job ID"
    "\n-v=<verb-num>         Logging verbosity: 0=CRIT 1=WARN 2=INFO 3=VERB 4=VVERB ..."

    "\n\nScheduler parameters:"
    "\n-ba=<num-ba>          Number of bounce alternatives per node (only relevant if -derandomize)"
    "\n-bm=<fp|ed>           Balancing mode (\"fp\": fixed-period, \"ed\": event-driven)"
    "\n-derandomize[=<0|1>]  Derandomize job bouncing and build a <num-ba>-regular message graph instead"
    "\n-jjp[=<0|1>]          Jitter job priorities to break ties during rebalancing"
    "\n-l=<load-factor>      Load factor to be aimed at (0 < l < 1)"
    "\n-p=<rebalance-period> Do balancing every t seconds (t > 0). With -bm=ed : minimum delay between balancings"
    "\n-r=<prob|bisec|floor> Mode of rounding of assignments in balancing"
    "\n                      (\"prob\": probabilistic, \"bisec\": iterative bisection, \"floor\" - always round down)"
    "\n-rto=<duration>       Request timeout: discard non-root job requests when older than <duration> seconds"
    "\n                      (0: no discarding)"

    "\n\nGlobal properties of jobs:"
    "\n-cg[=<0|1>]           Continuous growth of job demands (0: layer by layer, 1: node by node)"
    "\n-g=<growth-period>    Grow job demand exponentially every t seconds (t >= 0; 0: immediate growth)"
    "\n-job-cpu-limit=<x>    Timeout an instance after x cpu seconds (x >= 0; 0: no timeout)"
    "\n-job-wallclock-limit=<x> Timeout an instance after x seconds wall clock time (x >= 0; 0: no timeout)"
    "\n-md=<max-demand>      Limit any job's demand to some maximum value (int x >= 0; 0: no limit)"
    "\n-s=<comm-period>      Do job-internal communication every t seconds (t >= 0, 0: do not communicate)"

    "\n\nSAT solving application options:"
    "\n-aod[=<0|1>]          Add additional old diversifiers to Lingeling"
    "\n-cbbs=<size>          Clause buffer base size in integers (default: 1500)"
    "\n-cbdf=<factor>        Clause buffer discount factor: reduce buffer size per node by <factor> each depth"
    "\n                      (0 < factor <= 1.0; default: 1.0)"
    "\n-cfhl=<secs>          Set clause filter half life of clauses until forgotten (integer; 0: no forgetting)"
    "\n-fhlbd=<max-length>   Final hard LBD limit: After max. number of clause prod. increases, this MUST be fulfilled"
    "\n                      for any clause to be shared"
    "\n-fslbd=<max-length>   Final soft LBD limit: After max. number of clause prod. increases, this must be fulfilled"
    "\n                      for a clause to be shared except it has special solver-dependent qualities"
    "\n-icpr=<ratio>         Increase a solver's Clause Production when it fills less than <ratio> of its buffer"
    "\n                      (0 <= x < 1; 0: never increase)"
    "\n-ihlbd=<max-length>   Initial hard LBD limit: Before any clause prod. increase, this MUST be fulfilled for any"
    "\n                      clause to be shared"
    "\n-islbd=<max-length>   Initial soft LBD limit: Before any clause prod. increase, this must be fulfilled for a"
    "\n                      clause to be shared except it has special solver-dependent qualities"
    "\n-hmcl=<max-length>    Hard maximum clause length: Only share clauses up to some length (int x >= 0; 0: no limit)"
    "\n-phasediv[=<0|1>]     Do not diversify solvers based on phase; native diversification only"
    "\n-satsolver=<seq>      Sequence of SAT solvers to cycle through for each job, one character per solver:"
#ifdef MALLOB_USE_RESTRICTED
    "\n                      l=lingeling c=cadical g=glucose"
#else
    "\n                      l=lingeling c=cadical"
#endif
    "\n-smcl=<max-length>    Soft maximum clause length: Only share clauses up to some length (int x >= 0; 0: no limit)"
    "\n                      except a clause has special solver-dependent qualities";

/**
 * Taken from Hordesat:ParameterProcessor.h by Tomas Balyo.
 */
void Parameters::init(int argc, char** argv) {
    setDefaults();
    for (int i = 1; i < argc; i++) {
        char* arg = argv[i];
        if (arg[0] != '-') {
            continue;
        }
        char* eq = strchr(arg, '=');
        if (eq == NULL) {
            char* left = arg+1;
            auto it = _params.find(left);
            if (it != _params.end() && it->second == "0") it->second = "1";
            else _params[left];
        } else {
            *eq = 0;
            char* left = arg+1;
            char* right = eq+1;
            _params[left] = right;
        }
    }
    expand();
}

void Parameters::setDefaults() {
    setParam("aod", "0"); // add old diversifiers (to lgl)
    setParam("appmode", "fork"); // application mode (fork or thread)
    setParam("ba", "4"); // num bounce alternatives (only relevant if -derandomize)
    setParam("bm", "ed"); // event-driven balancing (ed = event-driven, fp = fixed-period)
    setParam("c", "1"); // num clients
    setParam("cbbs", "1500"); // clause buffer base size
    setParam("cbdf", "0.75"); // clause buffer discount factor
    setParam("cfhl", "60"); // clause buffer half life
    setParam("cg", "1"); // continuous growth
    setParam("colors", "0"); // colored terminal output
    setParam("delaymonkey", "0"); // Small chance for each MPI call to block for some random amount of time
    setParam("derandomize", "1"); // derandomize job bouncing
    setParam("g", "5.0"); // job demand growth interval
    //setParam("h"); setParam("help"); // print usage
    setParam("icpr", "0.8"); // increase clause production ratio
    setParam("jc", "4"); // job cache
    setParam("jjp", "1"); // jitter job priorities
    setParam("l", "0.95"); // load factor
    setParam("latencymonkey", "0"); // Block all MPI_Isend operations by a small randomized amount of time 
    setParam("log", "."); // logging directory
    setParam("lbc", "0"); // leaky bucket client parameter (0 = no leaky bucket, jobs enter by time) 
    setParam("md", "0"); // maximum demand per job (0 = no limit)
    setParam("mmpi", "0"); // monitor MPI
    setParam("mono", ""); // mono instance solving mode (if nonempty)
    setParam("phasediv", "1"); // Do phase-based diversification (in addition to native)
    setParam("p", "0.1"); // minimum interval between rebalancings (seconds)
    setParam("q", "0"); // no logging to stdout
    setParam("r", ROUNDING_BISECTION); // rounding of assignments (prob = probabilistic, bisec = iterative bisection)
    setParam("rto", "0"); // (job)requests timeout in seconds
    setParam("s", "1.0"); // job communication period (seconds)
    setParam("s2f", ""); // write solutions to file (file path, or empty string for no writing)
    setParam("satsolver", "l"); // which SAT solvers to cycle through
    //setParam("scenario", "<filename>"); // set base name of scenario(s) to be simulated
    setParam("sleep", "100"); // microsecs to sleep in between worker main loop cycles
    setParam("T", "0"); // total time to run the system (0 = no limit)
    setParam("t", "1"); // num threads per node
    setParam("td", "0.01"); // temperature decay for thermodyn. balancing
    setParam("job-cpu-limit", "0"); // resource limit per instance, in cpu seconds (0 = no limit)
    setParam("job-wallclock-limit", "0"); // time limit per instance, in seconds wall clock time (0 = no limit)
    setParam("v", "2"); // verbosity 0=CRIT 1=WARN 2=INFO 3=VERB 4=VVERB ...
    setParam("warmup", "0"); // warmup run
    setParam("yield", "0"); // yield manager thread when no new messages
    // {Initial, final} hard LBD (glue) limit
    setParam("ihlbd", "7");
    setParam("fhlbd", "7");
    // {Initial, final} soft LBD (glue) limit
    setParam("islbd", "2");
    setParam("fslbd", "7");
    // {Hard, soft} maximal clause length
    setParam("hmcl", "20");
    setParam("smcl", "5");
}

void Parameters::expand() {
    if (isNotNull("mono")) {
        // Single instance solving
        setParam("c", "0"); // no clients
        setParam("g", "0.0"); // instantaneous growth of job demand
        setParam("l", "1.0"); // full load factor
        setParam("md", "0"); // no limit of max. demand
        setParam("bm", "ed"); // event-driven balancing: do once, then never again
        setParam("p", "0.01"); // low balancing delay to immediately get full demand
    }
    // Expand global lbc definition to local lbc definitions where not overridden
    if (isSet("lbc")) {
        std::string lbc = getParam("lbc");
        for (int c = 0; c < getIntParam("c"); c++) {
            std::string key = ("lbc" + std::to_string(c));
            if (!isSet(key)) setParam(key.c_str(), lbc.c_str());
        }
    }
}

void Parameters::printUsage() const {
    log(V2_INFO, OPTIONS);
}

void Parameters::printParams() const {
    std::string out = "";
    std::map<std::string, std::string> sortedParams;
    for (const auto& [key, val] : _params) sortedParams[key] = val;
    for (const auto& it : sortedParams) {
        if (it.second.empty()) {
            out += it.first + ", ";
        } else {
            out += it.first + "=" + it.second + ", ";
        }
    }
    out = out.substr(0, out.size()-2);
    log(V2_INFO, "Program options: %s\n", out.c_str());
}

void Parameters::setParam(const char* name) {
    _params[name];
}

void Parameters::setParam(const char* name, const char* value) {
    _params[name] = value;
}

void Parameters::setParam(const std::string& name, const std::string& value) {
    _params[name] = value;
}

bool Parameters::isSet(const std::string& name) const {
    return _params.find(name) != _params.end();
}

bool Parameters::isNotNull(const std::string& name) const {
    auto it = _params.find(name);
    if (it == _params.end()) return false;
    return it->second != "0" && it->second != "";
}

std::string Parameters::getParam(const std::string& name, const std::string& defaultValue) const {
    if (isSet(name)) {
        return _params.at(name);
    } else {
        return defaultValue;
    }
}

std::string Parameters::getParam(const std::string& name) const {
    return getParam(name, "ndef");
}

int Parameters::getIntParam(const std::string& name, int defaultValue) const {
    if (isSet(name)) {
        return atoi(_params.at(name).c_str());
    } else {
        return defaultValue;
    }
}

const std::string& Parameters::operator[](const std::string& key) const {
    assert(isSet(key));
    return _params.at(key);
}

std::string& Parameters::operator[](const std::string& key) {
    return _params[key];
}

int Parameters::getIntParam(const std::string& name) const {
    assert(isSet(name));
    return atoi(_params.at(name).c_str());
}

double Parameters::getDoubleParam(const std::string& name, double defaultValue) const {
    if (isSet(name)) {
        return atof(_params.at(name).c_str());
    } else {
        return defaultValue;
    }
}

double Parameters::getDoubleParam(const std::string& name) const {
    assert(isSet(name));
    return atof(_params.at(name).c_str());
}

float Parameters::getFloatParam(const std::string& name, float defaultValue) const {
    return (float)getDoubleParam(name, defaultValue);
}
float Parameters::getFloatParam(const std::string& name) const {
    return (float)getDoubleParam(name);
}

char* const* Parameters::asCArgs(const char* execName) const {

    const char** argv = new const char*[_params.size()+2];
    argv[0] = execName;
    int i = 1;
    for (const auto& param : _params) {

        size_t argsize = 1 + param.first.size() + (!param.second.empty() ? 1 + param.second.size() : 0) + 1;
        char* arg = (char*) malloc(argsize * sizeof(char));
        arg[0] = '-';
        strncpy(arg+1, param.first.c_str(), param.first.size());
        if (!param.second.empty()) {
            arg[1+param.first.size()] = '=';
            strncpy(arg+(1+param.first.size()+1), param.second.c_str(), param.second.size());
        }
        arg[argsize-1] = '\0';

        argv[i] = arg;
        i++;
    }
    argv[i] = nullptr;
    return (char* const*) argv;
}
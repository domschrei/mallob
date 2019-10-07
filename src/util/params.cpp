
#include "assert.h"

#include "params.h"
#include "console.h"

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
  setParam("p", "5.0"); // rebalance period (seconds)
  setParam("l", "0.95"); // load factor
  setParam("c", "1"); // num clients
  setParam("t", "2"); // num threads per node
}

void Parameters::printUsage() {

    Console::log("Usage: mallob [options] <scenario>");
    Console::log("<scenario> : File path and name prefix for client scenario(s);");
    Console::log("             will parse <name>.0 for one client, ");
    Console::log("             <name>.0 and <name>.1 for two clients, ...");
    Console::log("Options:");
    Console::log("-p=<rebalance-period> Do global rebalancing every r seconds (r > 0)");
    Console::log("-l=<load-factor>      Load factor to be aimed at (0 < l < 1)");
    Console::log("-c=<num-clients>      Amount of client nodes (int c >= 1)");
    Console::log("-t=<num-threads>      Amount of worker threads per node (int t >= 1)");
}

string Parameters::getFilename() {
  return filename;
}

void Parameters::printParams() {
  for (map<string,string>::iterator it = params.begin(); it != params.end(); it++) {
    if (it->second.empty()) {
      cout << it->first << ", ";
    } else {
      cout << it->first << "=" << it->second << ", ";
    }
  }
  cout << "\n";
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

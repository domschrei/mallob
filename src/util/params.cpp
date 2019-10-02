
#include "params.h"

void Parameters::init(int argc, char** argv) {
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

float Parameters::getFloatParam(const string& name, float defaultValue) {
  if (isSet(name)) {
    return atof(params[name].c_str());
  } else {
    return defaultValue;
  }
}


#include "option.hpp"

#include <stdlib.h>
#include <string>
#include <iostream>

Option::Option(OptMap& map, GroupedOptionsList& groupedOpts, const std::string& id, const std::string& longid, const std::string& desc):
    id(id), longid(longid), desc(desc) {
    map[id] = this;
    groupedOpts.back()->map[id] = this;
}
bool Option::hasLongOption() const {
    return !longid.empty();
}
void Option::throwError(const std::string& specifics) {
    std::cout << "ERROR for option \"" << id << "\": " << specifics << std::endl;
    std::cout << "Description of \"" << id << "\": " << desc << std::endl;
    abort();
}

bool BoolOption::operator()() const {return val;}
void BoolOption::set(bool val) {this->val = val;}
std::string BoolOption::getValAsString() const {return val ? "1" : "0";}
void BoolOption::setValAsString(const std::string& valStr) {set(valStr != "0");}
void BoolOption::copyValue(const Option& other) {set( ((BoolOption&)other)() );}
const char* BoolOption::getTypeString() const {return "bool";}

int IntOption::operator()() const {return val;}
void IntOption::set(int val) {
    if (val < min) throwError(std::to_string(val) + " < " + std::to_string(min) + "(min)!");
    if (val > max) throwError(std::to_string(val) + " > " + std::to_string(max) + "(max)!");
    this->val = val;
}
bool IntOption::isNonzero() const {return val != 0;}
std::string IntOption::getValAsString() const {return std::to_string(val);}
void IntOption::setValAsString(const std::string& valStr) {set(atoi(valStr.c_str()));}
void IntOption::copyValue(const Option& other) {set( ((IntOption&)other)() );}
const char* IntOption::getTypeString() const {return "int";}

float FloatOption::operator()() const {return val;}
void FloatOption::set(float val) {
    if (val < min) throwError(std::to_string(val) + " < " + std::to_string(min) + "(min)!");
    if (val > max) throwError(std::to_string(val) + " > " + std::to_string(max) + "(max)!");
    this->val = val;
}
bool FloatOption::isNonzero() const {return val != 0;}
std::string FloatOption::getValAsString() const {
    std::string str = std::to_string(val);
    if (str.find('.') == std::string::npos) return str; 
    while (str[str.size()-1] == '0') str = str.substr(0, str.size()-1);
    if (str[str.size()-1] == '.') str = str.substr(0, str.size()-1);
    return str;
}
void FloatOption::setValAsString(const std::string& valStr) {set(atof(valStr.c_str()));}
void FloatOption::copyValue(const Option& other) {set( ((FloatOption&)other)() );}
const char* FloatOption::getTypeString() const {return "float";}

const std::string& StringOption::operator()() const {return val;}
void StringOption::set(const std::string& val) {this->val = val;}
bool StringOption::isSet() const {return !val.empty();}
std::string StringOption::getValAsString() const {return val;}
void StringOption::setValAsString(const std::string& valStr) {set(valStr);}
void StringOption::copyValue(const Option& other) {set( ((StringOption&)other)() );}
const char* StringOption::getTypeString() const {return "string";}

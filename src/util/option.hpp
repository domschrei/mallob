
#ifndef DOMPASCH_MALLOB_OPTION_HPP
#define DOMPASCH_MALLOB_OPTION_HPP

#include <string>
#include <assert.h>
#include <iostream>

#include "util/hashing.hpp"

struct Option;
typedef robin_hood::unordered_node_map<std::string, Option*> OptMap;

#define OPT_BOOL(member, id, longid, val, desc) BoolOption member = BoolOption(_map, id, longid, desc, val);
#define OPT_STRING(member, id, longid, val, desc) StringOption member = StringOption(_map, id, longid, desc, val);
#define OPT_INT(member, id, longid, val, min, max, desc) IntOption member = IntOption(_map, id, longid, desc, val, min, max);
#define OPT_FLOAT(member, id, longid, val, min, max, desc) FloatOption member = FloatOption(_map, id, longid, desc, val, min, max);

#define LARGE_INT 9999999
#define MAX_INT INT32_MAX

#define ROUNDING_BISECTION "bisec"
#define ROUNDING_PROBABILISTIC "prob"
#define ROUNDING_FLOOR "floor"

struct Option {
	std::string id;
	std::string longid;
	std::string desc;
	Option(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc):
		id(id), longid(longid), desc(desc) {
        map[id] = this;
    }
    bool hasLongOption() const {
        return !longid.empty();
    }
    virtual std::string getValAsString() const = 0;
    virtual void setValAsString(const std::string& valStr) = 0;
    virtual void copyValue(const Option& other) = 0;
    virtual const char* getTypeString() const = 0;
};

struct BoolOption : public Option {
	bool val;
	BoolOption(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc, bool val): 
        Option(map, id, longid, desc), val(val) {}
	bool operator()() const {return val;}
    void set(bool val) {this->val = val;}
    std::string getValAsString() const override {return val ? "1" : "0";}
    void setValAsString(const std::string& valStr) override {set(valStr != "0");}
    void copyValue(const Option& other) override {set( ((BoolOption&)other)() );}
    const char* getTypeString() const override {return "bool";}
};
struct IntOption : public Option {
	int val;
	int min;
	int max;
	IntOption(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc, int val, int min, int max): 
        Option(map, id, longid, desc), val(val), min(min), max(max) {}
	int operator()() const {return val;}
    void set(int val) {
        if (val < min) {
            std::cout << "Option " << id << ": " << val << " < " << min << "(min)!" << std::endl;
            abort();
        }
        if (val > max) {
            std::cout << "Option " << id << ": " << val << " > " << max << "(max)!" << std::endl;
            abort();
        }
        this->val = val;
    }
    bool isNonzero() const {return val != 0;}
    std::string getValAsString() const override {return std::to_string(val);}
    void setValAsString(const std::string& valStr) override {set(atoi(valStr.c_str()));}
    void copyValue(const Option& other) override {set( ((IntOption&)other)() );}
    const char* getTypeString() const override {return "int";}
};
struct FloatOption : public Option {
	float val;
	float min;
	float max;
	FloatOption(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc, float val, float min, float max): 
        Option(map, id, longid, desc), val(val), min(min), max(max) {}
	float operator()() const {return val;}
    void set(float val) {
        if (val < min) {
            std::cout << "Option " << id << ": " << val << " < " << min << "(min)!" << std::endl;
            abort();
        }
        if (val > max) {
            std::cout << "Option " << id << ": " << val << " > " << max << "(max)!" << std::endl;
            abort();
        }
        this->val = val;
    }
    bool isNonzero() const {return val != 0;}
    std::string getValAsString() const override {
        std::string str = std::to_string(val);
        if (str.find('.') == std::string::npos) return str; 
        while (str[str.size()-1] == '0') str = str.substr(0, str.size()-1);
        if (str[str.size()-1] == '.') str = str.substr(0, str.size()-1);
        return str;
    }
    void setValAsString(const std::string& valStr) override {set(atof(valStr.c_str()));}
    void copyValue(const Option& other) override {set( ((FloatOption&)other)() );}
    const char* getTypeString() const override {return "float";}
};
struct StringOption : public Option {
	std::string val;
	StringOption(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc, const std::string& val): 
        Option(map, id, longid, desc), val(val) {}
	const std::string& operator()() const {return val;}
    void set(const std::string& val) {this->val = val;}
    bool isSet() const {return !val.empty();}
    std::string getValAsString() const override {return val;}
    void setValAsString(const std::string& valStr) override {set(valStr);}
    void copyValue(const Option& other) override {set( ((StringOption&)other)() );}
    const char* getTypeString() const override {return "string";}
};

#endif

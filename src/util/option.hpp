
#ifndef DOMPASCH_MALLOB_OPTION_HPP
#define DOMPASCH_MALLOB_OPTION_HPP

#include <string>

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
	Option(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc);
    bool hasLongOption() const;
    virtual std::string getValAsString() const = 0;
    virtual void setValAsString(const std::string& valStr) = 0;
    virtual void copyValue(const Option& other) = 0;
    virtual const char* getTypeString() const = 0;
};

struct BoolOption : public Option {
	bool val;
	BoolOption(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc, bool val): 
        Option(map, id, longid, desc), val(val) {}
	bool operator()() const;
    void set(bool val);
    std::string getValAsString() const override;
    void setValAsString(const std::string& valStr) override;
    void copyValue(const Option& other) override;
    const char* getTypeString() const override;
};
struct IntOption : public Option {
	int val;
	int min;
	int max;
	IntOption(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc, int val, int min, int max): 
        Option(map, id, longid, desc), val(val), min(min), max(max) {}
	int operator()() const;
    void set(int val);
    bool isNonzero() const;
    std::string getValAsString() const override;
    void setValAsString(const std::string& valStr) override;
    void copyValue(const Option& other) override;
    const char* getTypeString() const override;
};
struct FloatOption : public Option {
	float val;
	float min;
	float max;
	FloatOption(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc, float val, float min, float max): 
        Option(map, id, longid, desc), val(val), min(min), max(max) {}
	float operator()() const;
    void set(float val);
    bool isNonzero() const;
    std::string getValAsString() const override;
    void setValAsString(const std::string& valStr) override;
    void copyValue(const Option& other) override;
    const char* getTypeString() const override;
};
struct StringOption : public Option {
	std::string val;
	StringOption(OptMap& map, const std::string& id, const std::string& longid, const std::string& desc, const std::string& val): 
        Option(map, id, longid, desc), val(val) {}
	const std::string& operator()() const;
    void set(const std::string& val);
    bool isSet() const;
    std::string getValAsString() const override;
    void setValAsString(const std::string& valStr) override;
    void copyValue(const Option& other) override;
    const char* getTypeString() const override;
};

#endif

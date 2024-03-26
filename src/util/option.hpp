
#ifndef DOMPASCH_MALLOB_OPTION_HPP
#define DOMPASCH_MALLOB_OPTION_HPP

#include <stdint.h>
#include <string>
#include <list>

#include "util/hashing.hpp"
#include "util/robin_hood.hpp"

struct Option;

typedef robin_hood::unordered_node_map<std::string, Option*> OptMap;
struct OptGroup {
    std::string groupId; std::string desc; OptMap map;
    OptGroup(std::list<OptGroup*>& groupedOpts, const std::string& groupId, const std::string& desc) :
            groupId(groupId), desc(desc) {
        groupedOpts.push_back(this);
    }
};
typedef std::list<OptGroup*> GroupedOptionsList;

#define OPTION_GROUP(member, id, desc) OptGroup member = OptGroup(_grouped_list, id, desc);
#define OPT_BOOL(member, id, longid, val, desc) BoolOption member = BoolOption(_global_map, _grouped_list, id, longid, desc, val);
#define OPT_STRING(member, id, longid, val, desc) StringOption member = StringOption(_global_map, _grouped_list, id, longid, desc, val);
#define OPT_INT(member, id, longid, val, min, max, desc) IntOption member = IntOption(_global_map, _grouped_list, id, longid, desc, val, min, max);
#define OPT_FLOAT(member, id, longid, val, min, max, desc) FloatOption member = FloatOption(_global_map, _grouped_list, id, longid, desc, val, min, max);

#define LARGE_INT 9999999
#define MAX_INT INT32_MAX

struct Option {
	std::string id;
	std::string longid;
	std::string desc;
	Option(OptMap& map, GroupedOptionsList& groupedOpts, const std::string& id, const std::string& longid, const std::string& desc);
    bool hasLongOption() const;
    virtual std::string getValAsString() const = 0;
    virtual void setValAsString(const std::string& valStr) = 0;
    virtual void copyValue(const Option& other) = 0;
    virtual const char* getTypeString() const = 0;
};

struct BoolOption : public Option {
	bool val;
	BoolOption(OptMap& map, GroupedOptionsList& groupedOpts, const std::string& id, const std::string& longid, const std::string& desc, bool val): 
        Option(map, groupedOpts, id, longid, desc), val(val) {}
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
	IntOption(OptMap& map, GroupedOptionsList& groupedOpts, const std::string& id, const std::string& longid, const std::string& desc, int val, int min, int max): 
        Option(map, groupedOpts, id, longid, desc), val(val), min(min), max(max) {}
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
	FloatOption(OptMap& map, GroupedOptionsList& groupedOpts, const std::string& id, const std::string& longid, const std::string& desc, float val, float min, float max): 
        Option(map, groupedOpts, id, longid, desc), val(val), min(min), max(max) {}
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
	StringOption(OptMap& map, GroupedOptionsList& groupedOpts, const std::string& id, const std::string& longid, const std::string& desc, const std::string& val): 
        Option(map, groupedOpts, id, longid, desc), val(val) {}
	const std::string& operator()() const;
    void set(const std::string& val);
    bool isSet() const;
    std::string getValAsString() const override;
    void setValAsString(const std::string& valStr) override;
    void copyValue(const Option& other) override;
    const char* getTypeString() const override;
};

#endif

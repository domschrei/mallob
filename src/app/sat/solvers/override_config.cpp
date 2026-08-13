// Implementation of the SolverOverrideConfig portfolio-configuration-override
// interface declared in override_config.hpp.
//
// Initial version generated via Claude Sonnet 5 (Medium), 2026-08-12

#include "override_config.hpp"
#include "app/sat/data/portfolio_sequence.hpp"
#include "util/distribution.hpp"
#include "util/random.hpp"
#include "util/sys/fileutils.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;

// ---------------------------------------------------------------------
// Backend name <-> enum
// ---------------------------------------------------------------------

SolverBackendType parseSolverBackendType(const std::string& name) {
    if (name == "Kissat") return SolverBackendType::KISSAT;
    if (name == "CaDiCaL") return SolverBackendType::CADICAL;
    if (name == "Lingeling") return SolverBackendType::LINGELING;
    throw std::runtime_error("Unknown solver backend '" + name +
                              "' (expected one of: Kissat, CaDiCaL, Lingeling)");
}

std::string toString(SolverBackendType backend) {
    switch (backend) {
        case SolverBackendType::KISSAT: return "Kissat";
        case SolverBackendType::CADICAL: return "CaDiCaL";
        case SolverBackendType::LINGELING: return "Lingeling";
        default:
            throw std::runtime_error("Unsupported solver backend type for solver override config");
    }
}

// ---------------------------------------------------------------------
// Selector implementations
// ---------------------------------------------------------------------

bool TrueSelector::matches(int /*index*/, PortfolioSequence::Flavour, int /*seed*/) const { return true; }

RandomSelector::RandomSelector(double prob) : _prob(prob) {}

bool RandomSelector::matches(int index, PortfolioSequence::Flavour flavour, int seed) const {
    SplitMix64Rng rng(index * seed);
    return rng.randomInRange(0, 1) <= _prob;
}

FlavourSelector::FlavourSelector(PortfolioSequence::Flavour flavour) : _flavour(flavour) {}

bool FlavourSelector::matches(int index, PortfolioSequence::Flavour flavour, int seed) const {
    return _flavour == flavour;
}

RangeSelector::RangeSelector(int from, int to) : _from(from), _to(to) {}

bool RangeSelector::matches(int index, PortfolioSequence::Flavour flavour, int seed) const {
    return index >= _from && index <= _to;
}

ScatterSelector::ScatterSelector(int start, int step) : _start(start), _step(step) {}

bool ScatterSelector::matches(int index, PortfolioSequence::Flavour flavour, int seed) const {
    if (index < _start) return false;
    if (_step == 0) return index == _start;
    return (index - _start) % _step == 0;
}

NotSelector::NotSelector(std::unique_ptr<Selector> inner) : _inner(std::move(inner)) {}

bool NotSelector::matches(int index, PortfolioSequence::Flavour flavour, int seed) const {
    return !_inner->matches(index, flavour, seed);
}

AndSelector::AndSelector(std::vector<std::unique_ptr<Selector>> children)
    : _children(std::move(children)) {}

bool AndSelector::matches(int index, PortfolioSequence::Flavour flavour, int seed) const {
    for (const auto& child : _children) {
        if (!child->matches(index, flavour, seed)) return false;
    }
    return true;
}

OrSelector::OrSelector(std::vector<std::unique_ptr<Selector>> children)
    : _children(std::move(children)) {}

bool OrSelector::matches(int index, PortfolioSequence::Flavour flavour, int seed) const {
    for (const auto& child : _children) {
        if (child->matches(index, flavour, seed)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------
// Selector parsing
// ---------------------------------------------------------------------

namespace {

int requireInt(const json& j, const char* key, const std::string& context) {
    if (!j.contains(key)) {
        throw std::runtime_error(context + ": missing required field '" + key + "'");
    }
    const json& value = j.at(key);
    if (!value.is_number_integer()) {
        throw std::runtime_error(context + ": field '" + key + "' must be an integer");
    }
    return value.get<int>();
}
double requireDouble(const json& j, const char* key, const std::string& context) {
    if (!j.contains(key)) {
        throw std::runtime_error(context + ": missing required field '" + key + "'");
    }
    const json& value = j.at(key);
    if (!value.is_number_float()) {
        throw std::runtime_error(context + ": field '" + key + "' must be a float");
    }
    return value.get<double>();
}

} // namespace

std::unique_ptr<Selector> parseSelector(const json& j) {
    if (!j.is_object()) {
        throw std::runtime_error("Selector must be a JSON object");
    }
    if (!j.contains("type") || !j.at("type").is_string()) {
        throw std::runtime_error("Selector is missing a string 'type' field");
    }
    const std::string type = j.at("type").get<std::string>();

    if (type == "true") {
        return std::make_unique<TrueSelector>();
    }

    if (type == "flavour") {
        if (!j.contains("value"))
            throw std::runtime_error("Flavour selector is missing a string 'value' field");
        if (!j.at("value").is_string())
            throw std::runtime_error("Flavour selector 'value' field must be a string");
        if (j.at("value") == "default")
            return std::make_unique<FlavourSelector>(PortfolioSequence::Flavour::DEFAULT);
        else if (j.at("value") == "sat")
            return std::make_unique<FlavourSelector>(PortfolioSequence::Flavour::SAT);
        else if (j.at("value") == "unsat")
            return std::make_unique<FlavourSelector>(PortfolioSequence::Flavour::UNSAT);
        else if (j.at("value") == "plain")
            return std::make_unique<FlavourSelector>(PortfolioSequence::Flavour::PLAIN);
        else if (j.at("value") == "preprocess")
            return std::make_unique<FlavourSelector>(PortfolioSequence::Flavour::PREPROCESS);
        else
            throw std::runtime_error("Flavour selector has invalid 'value' field "
                + j.at("value").get<std::string>());
    }

    if (type == "random") {
        return std::make_unique<RandomSelector>(requireDouble(j, "chance", "random selector"));
    }

    if (type == "range") {
        const int from = requireInt(j, "from", "range selector");
        const int to = requireInt(j, "to", "range selector");
        if (from > to) {
            throw std::runtime_error("range selector: 'from' (" + std::to_string(from) +
                                      ") must be <= 'to' (" + std::to_string(to) + ")");
        }
        return std::make_unique<RangeSelector>(from, to);
    }

    if (type == "scatter") {
        const int start = requireInt(j, "start", "scatter selector");
        const int step = requireInt(j, "step", "scatter selector");
        if (step < 0) {
            throw std::runtime_error("scatter selector: 'step' must be >= 0");
        }
        return std::make_unique<ScatterSelector>(start, step);
    }

    if (type == "not") {
        if (!j.contains("selector")) {
            throw std::runtime_error("not selector: missing 'selector' field");
        }
        return std::make_unique<NotSelector>(parseSelector(j.at("selector")));
    }

    if (type == "and" || type == "or") {
        if (!j.contains("selectors") || !j.at("selectors").is_array()) {
            throw std::runtime_error(type + " selector: missing/invalid 'selectors' array");
        }
        std::vector<std::unique_ptr<Selector>> children;
        for (const auto& childJson : j.at("selectors")) {
            children.push_back(parseSelector(childJson));
        }
        if (children.empty()) {
            throw std::runtime_error(type + " selector: 'selectors' array must not be empty");
        }
        if (type == "and") {
            return std::make_unique<AndSelector>(std::move(children));
        }
        return std::make_unique<OrSelector>(std::move(children));
    }

    throw std::runtime_error("Unknown selector type '" + type +
                              "' (expected: true, range, scatter, not, and, or)");
}

// ---------------------------------------------------------------------
// SolverConfig
// ---------------------------------------------------------------------

SettingsList SolverOverrideConfig::parseSettings(const json& settingsJson) {
    if (!settingsJson.is_array()) {
        throw std::runtime_error("'settings' must be a JSON array");
    }
    SettingsList settings;
    for (auto it = settingsJson.begin(); it != settingsJson.end(); ++it) {
        Setting setting;
        const std::string& type = it->at("type");
        const std::string& key = it->at("key");
        if (type == "set") setting.type = Setting::SET;
        else if (type == "add") setting.type = Setting::ADD;
        else if (type == "configure") setting.type = Setting::CONFIGURE;
        setting.key = key;
        const json& value = it->at("value");
        if (value.is_string()) {
            setting.val = value.get<std::string>();
        } else if (value.is_number_integer()) {
            setting.val = value.get<long long>();
        } else if (value.is_number_float()) {
            setting.val = value.get<double>();
        } else {
            throw std::runtime_error("setting '" + key +
                                      "' has unsupported value type (must be string or number)");
        }
        if (it->contains("min")) setting.min = it->at("min").get<long long>();
        if (it->contains("max")) setting.max = it->at("max").get<long long>();
        settings.push_back(std::move(setting));
    }
    return settings;
}

Rule SolverOverrideConfig::parseRule(const json& ruleJson) {
    if (!ruleJson.is_object()) {
        throw std::runtime_error("Each rule must be a JSON object");
    }
    if (!ruleJson.contains("backend") || !ruleJson.at("backend").is_string()) {
        throw std::runtime_error("Rule is missing a string 'backend' field");
    }
    if (!ruleJson.contains("selector")) {
        throw std::runtime_error("Rule is missing a 'selector' field");
    }
    if (!ruleJson.contains("settings")) {
        throw std::runtime_error("Rule is missing a 'settings' field");
    }

    Rule rule;
    rule.backend = parseSolverBackendType(ruleJson.at("backend").get<std::string>());
    rule.selector = parseSelector(ruleJson.at("selector"));
    rule.settings = parseSettings(ruleJson.at("settings"));
    return rule;
}

void SolverOverrideConfig::parseSingleFile(const std::string& filePath) {
    std::ifstream in(filePath);
    if (!in.is_open()) {
        throw std::runtime_error("Could not open configuration file: " + filePath);
    }

    json root;
    try {
        in >> root;
    } catch (const json::parse_error& e) {
        throw std::runtime_error("Failed to parse JSON file '" + filePath + "': " + e.what());
    }

    if (!root.is_object() || !root.contains("rules") || !root.at("rules").is_array()) {
        throw std::runtime_error("Configuration file '" + filePath +
                                  "' must be a JSON object with a 'rules' array");
    }

    std::size_t ruleIndex = 0;
    for (const auto& ruleJson : root.at("rules")) {
        try {
            _rules.push_back(parseRule(ruleJson));
        } catch (const std::exception& e) {
            std::ostringstream oss;
            oss << "Error in '" << filePath << "', rules[" << ruleIndex << "]: " << e.what();
            throw std::runtime_error(oss.str());
        }
        ++ruleIndex;
    }

    if (!root.contains("samplers")) return;
    if (!root.at("samplers").is_array()) {
        throw std::runtime_error("Configuration file '" + filePath +
                                  "' : 'samplers' JSON object must be an array");
    }
    for (const auto& samplerJson : root.at("samplers")) {
        if (!samplerJson.contains("name") || !samplerJson.contains("distribution")) {
            throw std::runtime_error("Configuration file '" + filePath +
                                  "' : 'samplers' array entry must have 'name' and 'distribution' fields");
        }
        RandomDistribution rd;
        RandomDistribution::parse(samplerJson.at("distribution"), &rd);
        _samplers[samplerJson.at("name").get<std::string>()] = std::move(rd);
    }
}

void SolverOverrideConfig::parseFromJson(const std::vector<std::string>& filePaths) {
    for (const auto& filePath : filePaths) {
        parseSingleFile(filePath);
    }
}

void SolverOverrideConfig::parseFromDirsAndFiles(const std::string& dirList, const std::string& fileList) {
    std::vector<std::string> jsonPaths;
	if (!dirList.empty()) {
		std::vector<std::string> dirs;
		std::stringstream ss(dirList);
		std::string str;
		while (getline(ss, str, ',')) {
			dirs.push_back(str);
		}
		for (const auto& dir : dirs) {
			auto globbedFiles = FileUtils::glob(dir + "/*.json");
			for (const auto& f : globbedFiles) jsonPaths.push_back(f);
		}
	}
	if (!fileList.empty()) {
		std::stringstream ss(fileList);
		std::string str;
		while (getline(ss, str, ',')) {
			jsonPaths.push_back(str);
		}
	}
    parseFromJson(jsonPaths);
}

SettingsList SolverOverrideConfig::getConfigurationOverrides(SolverBackendType backend,
        PortfolioSequence::Flavour flavour, int index, int randomSeed) const {
    SettingsList result;
    for (const auto& rule : _rules) {
        if (rule.backend != backend) continue;
        if (!rule.selector->matches(index, flavour, index + randomSeed)) continue;
        for (auto setting : rule.settings) {
            // sample from random distribution where necessary
            if (setting.val.index() == 0) { // string
                std::string valStr = std::get<0>(setting.val);
                if (valStr.rfind("sample ", 0) == 0) { // pos=0 limits the search to the prefix
                    // s starts with prefix
                    valStr = valStr.substr(7); // everything after "sample "
                    if (!_samplers.count(valStr)) {
                        throw std::runtime_error("Invalid sampler \"" + valStr + "\" specified");
                    }
                    auto& dist = _samplers.at(valStr);
                    long long sampled = dist.sample(index + randomSeed++);
                    setting.val = SettingValue(sampled);
                }
            }
            // add setting
            result.push_back(setting);
        }
    }
    return result;
}

void SolverOverrideConfig::clear() { _rules.clear(); }

std::size_t SolverOverrideConfig::ruleCount() const { return _rules.size(); }

// Configuration-override interface for SAT solver portfolios.
//
// Users supply one or more JSON files, each containing a list of "rules".
// Each rule says: for solver threads of a given BACKEND whose (0-based)
// index matches a given SELECTOR, apply a given set SETTINGS.
//
// JSON schema (informal):
//
// {
//   "rules": [
//     {
//       "backend": <"Kissat" | "CaDiCaL" | "Lingeling">,
//       "selector": <selector>,
//       "settings": [ 
//         {"type": "set", "key": "some-option", "value": "value"}, 
//         {"type": "set", "key": "some-option", "value": "sample R1"}, 
//             // - value "sample <dist>" draws a sample from distribution R1
//             //   (see below at "samplers")
//         {"type": "add", "key": "another-option", "value": 42,
//           "min": 1, "max": 1000},
//             // - optional min/max fields clamp *final* value to [min, max]
//         {"type": "configure", "key": "configure", "value": "plain"}
//       ]
//     },
//     ...
//   ],
//   "samplers": [
//     {
//       "name": "R1",
//       "distribution": {
//         "type": <"constant" | "uniform" | "normal" | "exponential">
//         "params": [0, 10, -100000, 100000]
//             // type == constant: [val]
//             // type == uniform: [min, max]
//             // type == normal: [mean, stddev, min, max]
//             // type == exponential: [lambda]
//       }
//     },
//     ...
//   ]
// }
//
// A <selector> is one of:
//   { "type": "true" }
//   { "type": "flavour", "value": <"default" | "sat" | "unsat" | "plain" | "preprocess"> }
//   { "type": "random",  "chance": <0..1> }
//   { "type": "range",   "from": <int>, "to": <int> }                // inclusive [i,j]
//   { "type": "scatter", "start": <int>, "step": <int> }             // {start + x*step : x >= 0}
//   { "type": "not",     "selector": <selector> }
//   { "type": "and",     "selectors": [ <selector>, ... ] }
//   { "type": "or",      "selectors": [ <selector>, ... ] }
//
// A setting value is either a JSON string or a JSON number (integer or
// floating point).
//
// Multiple files are parsed in the order given, and within a file rules are
// evaluated in array order. All matching rules for a (backend, index) pair
// contribute their settings, in the specified order. This lets
// users layer a "defaults" file and an "overrides" file, for example.
//
// Initial version generated via Claude Sonnet 5 (Medium), 2026-08-12

#pragma once

#include "app/sat/data/portfolio_sequence.hpp"
#include "robin_map.h"
#include "util/distribution.hpp"
#include "util/json.hpp"
#include <climits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// The three supported SAT solver backends used by the portfolio solver.
typedef PortfolioSequence::BaseSolver SolverBackendType;

// Convert to/from the strings used in the JSON files ("Kissat", "CaDiCaL",
// "Lingeling"). Throws std::runtime_error on an unrecognized name.
SolverBackendType parseSolverBackendType(const std::string& name);
std::string toString(SolverBackendType backend);

// The value of a single configuration setting. Integers and floating point
// numbers are kept distinct so callers can format them appropriately when
// handing them to the underlying solver's option-setting API.
using SettingValue = std::variant<std::string, long long, double>;

struct Setting {
    enum Type {CONFIGURE, SET, ADD} type;
    std::string key;
    SettingValue val;
    long long min {LLONG_MIN};
    long long max {LLONG_MAX};
};

// Ordered by key so that iterating the result of getConfigurationOverrides()
// is deterministic (handy for logging/debugging).
using SettingsList = std::vector<Setting>;

// ---------------------------------------------------------------------
// Selector hierarchy
// ---------------------------------------------------------------------
//
// A Selector decides, for a given 0-based solver-thread index, whether a
// rule applies. The hierarchy is intentionally polymorphic (rather than a
// tagged union/variant) because NOT/AND/OR are naturally recursive and this
// keeps evaluation and parsing simple.

class Selector {
public:
    virtual ~Selector() = default;
    virtual bool matches(int index, PortfolioSequence::Flavour flavour, int seed) const = 0;
};

// Matches every index.
class TrueSelector final : public Selector {
public:
    bool matches(int index, PortfolioSequence::Flavour flavour, int seed) const override;
};

class FlavourSelector final : public Selector {
public:
    FlavourSelector(PortfolioSequence::Flavour flavour);
    bool matches(int index, PortfolioSequence::Flavour flavour, int seed) const override;

private:
    PortfolioSequence::Flavour _flavour;
};

class RandomSelector final : public Selector {
public:
    RandomSelector(double prob);
    bool matches(int index, PortfolioSequence::Flavour flavour, int seed) const override;

private:
    double _prob;
};

// Matches the closed interval [from, to].
class RangeSelector final : public Selector {
public:
    RangeSelector(int from, int to);
    bool matches(int index, PortfolioSequence::Flavour flavour, int seed) const override;

private:
    int _from;
    int _to;
};

// Matches the arithmetic progression {start + x*step : x = 0, 1, 2, ...}.
// A step of 0 degenerates to matching only `start` itself.
// A negative step is rejected at parse time (see parseSelector).
class ScatterSelector final : public Selector {
public:
    ScatterSelector(int start, int step);
    bool matches(int index, PortfolioSequence::Flavour flavour, int seed) const override;

private:
    int _start;
    int _step;
};

class NotSelector final : public Selector {
public:
    explicit NotSelector(std::unique_ptr<Selector> inner);
    bool matches(int index, PortfolioSequence::Flavour flavour, int seed) const override;

private:
    std::unique_ptr<Selector> _inner;
};

class AndSelector final : public Selector {
public:
    explicit AndSelector(std::vector<std::unique_ptr<Selector>> children);
    bool matches(int index, PortfolioSequence::Flavour flavour, int seed) const override;

private:
    std::vector<std::unique_ptr<Selector>> _children;
};

class OrSelector final : public Selector {
public:
    explicit OrSelector(std::vector<std::unique_ptr<Selector>> children);
    bool matches(int index, PortfolioSequence::Flavour flavour, int seed) const override;

private:
    std::vector<std::unique_ptr<Selector>> _children;
};

// Parses a <selector> JSON object into a Selector tree. Throws
// std::runtime_error on malformed input.
std::unique_ptr<Selector> parseSelector(const nlohmann::json& json);

// ---------------------------------------------------------------------
// Rule
// ---------------------------------------------------------------------

struct Rule {
    SolverBackendType backend;
    std::shared_ptr<Selector> selector;
    SettingsList settings;
};

// ---------------------------------------------------------------------
// SolverConfig
// ---------------------------------------------------------------------

class SolverOverrideConfig {
public:
    void parseFromDirsAndFiles(const std::string& dirList, const std::string& fileList);

    // Parses and appends the rules found in each file, in order. May be
    // called multiple times; rules accumulate (they are not cleared first).
    // Throws std::runtime_error if a file cannot be opened or its content
    // does not match the expected schema.
    void parseFromJson(const std::vector<std::string>& filePaths);

    // Returns the merged configuration overrides that apply to the solver
    // thread of the given backend at the given (0-based) index. Rules are
    // evaluated in the order they were parsed; later matching rules
    // overwrite settings from earlier matching rules with the same key.
    SettingsList getConfigurationOverrides(SolverBackendType backend,
        PortfolioSequence::Flavour flavour, int index, int randomSeed = 0) const;

    // Removes all previously parsed rules.
    void clear();

    // Number of currently loaded rules (mainly useful for testing/logging).
    std::size_t ruleCount() const;

private:
    void parseSingleFile(const std::string& filePath);
    static Rule parseRule(const nlohmann::json& ruleJson);
    static SettingsList parseSettings(const nlohmann::json& settingsJson);

    std::vector<Rule> _rules;
    tsl::robin_map<std::string, RandomDistribution> _samplers;
};

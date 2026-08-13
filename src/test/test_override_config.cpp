// Small sanity-check / usage demo for SolverConfig.
// Initial version generated via Claude Sonnet 5 (Medium), 2026-08-12

#include "app/sat/data/portfolio_sequence.hpp"
#include "app/sat/solvers/override_config.hpp"
#include <iostream>
#include <variant>

static void printOverrides(const SolverOverrideConfig& config, SolverBackendType backend, int index) {
    std::cout << toString(backend) << "[" << index << "] -> {";
    bool first = true;
    for (const auto& setting : config.getConfigurationOverrides(
                backend, PortfolioSequence::DEFAULT, index)) {
        if (!first) std::cout << ", ";
        first = false;
        if (setting.type == Setting::SET) std::cout << "SET: ";
        if (setting.type == Setting::ADD) std::cout << "ADD: ";
        if (setting.type == Setting::CONFIGURE) std::cout << "CONF: ";
        std::cout << setting.key << "=";
        std::visit([](const auto& v) { std::cout << v; }, setting.val);
    }
    std::cout << "}\n";
}

int main() {
    SolverOverrideConfig config;
    try {
        config.parseFromJson({"templates/solver_override_example_config.json"});
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse configuration: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Loaded " << config.ruleCount() << " rules\n\n";

    // Kissat threads 0..6: rule 1 (TRUE) applies to all; rule 2 (range 0-3)
    // overrides restartint for 0..3; rule 5 (OR) overrides 'phase' back to
    // "original" for index 5.
    for (int i = 0; i <= 6; ++i) {
        printOverrides(config, SolverBackendType::KISSAT, i);
    }
    std::cout << "\n";

    // CaDiCaL threads 0..5: only odd indices (1,3,5,...) get the 'walk'/
    // 'elimreleff' settings.
    for (int i = 0; i <= 5; ++i) {
        printOverrides(config, SolverBackendType::CADICAL, i);
    }
    std::cout << "\n";

    // Lingeling threads 0..8: indices that are multiples of 3 EXCEPT 0..2,
    // i.e. 3, 6 get 'plain' (0 is excluded by the NOT range).
    for (int i = 0; i <= 8; ++i) {
        printOverrides(config, SolverBackendType::LINGELING, i);
    }

    return 0;
}

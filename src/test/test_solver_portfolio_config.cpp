// Small sanity-check / usage demo for SolverPortfolioConfig.
// Initial version generated via Claude Sonnet 5 (Medium), 2026-08-12

#include "app/sat/data/portfolio_sequence.hpp"
#include "app/sat/solvers/solver_portfolio_config.hpp"
#include "util/sys/timer.hpp"
#include <iostream>
#include <variant>

static void printSettings(const SolverPortfolioConfig& config, SolverBackendType backend, int index) {
    std::cout << toString(backend) << "[" << index << "] -> {";
    bool first = true;
    for (const auto& setting : config.getConfigurationSettings(
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

static void checkTiming() {
    auto t = Timer::elapsedSeconds();
    SolverPortfolioConfig config;
    try {
        config.parseFromDirsAndFiles("config/sat/base/,config/sat/", "");
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse configuration: " << e.what() << "\n";
        return;
    }
    t = Timer::elapsedSeconds() - t;
    std::cout << "Loaded " << config.ruleCount() << " rules within " << t << "s\n\n";
}

int main() {
    Timer::init();

    SolverPortfolioConfig config;
    try {
        config.parseFromJson({"config/sat/example.json"});
    } catch (const std::exception& e) {
        std::cerr << "Failed to parse configuration: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Loaded " << config.ruleCount() << " rules\n\n";

    // Kissat threads 0..6: rule 1 (TRUE) applies to all; rule 2 (range 0-3)
    // overrides restartint for 0..3; rule 5 (OR) overrides 'phase' back to
    // "original" for index 5.
    for (int i = 0; i <= 6; ++i) {
        printSettings(config, SolverBackendType::KISSAT, i);
    }
    std::cout << "\n";

    // CaDiCaL threads 0..5: only odd indices (1,3,5,...) get the 'walk'/
    // 'elimreleff' settings.
    for (int i = 0; i <= 5; ++i) {
        printSettings(config, SolverBackendType::CADICAL, i);
    }
    std::cout << "\n";

    // Lingeling threads 0..8: indices that are multiples of 3 EXCEPT 0..2,
    // i.e. 3, 6 get 'plain' (0 is excluded by the NOT range).
    for (int i = 0; i <= 8; ++i) {
        printSettings(config, SolverBackendType::LINGELING, i);
    }

    checkTiming();

    return 0;
}

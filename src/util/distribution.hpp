
#pragma once

#include "util/json.hpp"
#include <iostream>
#include <variant>
#include <vector>
#include <random>

struct RandomDistribution {
    enum Type {CONSTANT, UNIFORM, EXPONENTIAL, NORMAL} type;
    std::vector<double> params;
    mutable std::mt19937 _rng;
    mutable std::variant<
        std::uniform_real_distribution<double>,
        std::exponential_distribution<double>,
        std::normal_distribution<double>
    > _distribution;

    RandomDistribution() {}
    RandomDistribution(std::mt19937& rng) : _rng(rng) {}
    RandomDistribution(int seed) : _rng(seed) {}

    void configure(Type type, const std::vector<double>& params) {
        this->type = type;
        this->params = params;
        switch (type) {
        case UNIFORM:
            _distribution = std::uniform_real_distribution<double>(params[0], params[1]);
            break;
        case EXPONENTIAL:
            _distribution = std::exponential_distribution<double>(params[0]);
            break;
        case NORMAL:
            _distribution = std::normal_distribution<double>(params[0], params[1]);
            break;
        default:
            break;
        }
    }

    double sample(long seed = 0) const {
        if (seed != 0) {
            _rng = std::mt19937(seed); // requires "mutable"
        }
        switch (type) {
        case CONSTANT:
            return params[0];
        case UNIFORM:
            return std::get<0>(_distribution)(_rng);
        case EXPONENTIAL:
            return std::get<1>(_distribution)(_rng);
        case NORMAL:
            double result = std::get<2>(_distribution)(_rng);
            if (params.size() > 2) result = std::max(result, params[2]);
            if (params.size() > 3) result = std::min(result, params[3]);
            return result;
        }
        return 0;
    }

    static void parse(const nlohmann::json& json, RandomDistribution* dist) {
        std::vector<double> params = json.at("params").get<std::vector<double>>();
        RandomDistribution::Type type;
        if (json.at("type") == "constant") type = RandomDistribution::CONSTANT;
        else if (json.at("type") == "uniform") type = RandomDistribution::UNIFORM;
        else if (json.at("type") == "exponential") type = RandomDistribution::EXPONENTIAL;
        else if (json.at("type") == "normal") type = RandomDistribution::NORMAL;
        else {
            std::cout << "[ERROR] \"" << json["type"] << "\" is not a valid distribution type!" << std::endl;
            abort();
        }
        dist->configure(type, params);
    }
};

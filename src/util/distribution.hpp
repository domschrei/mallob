
#pragma once

#include <vector>
#include <random>

struct Distribution {
    enum Type {CONSTANT, UNIFORM, EXPONENTIAL, NORMAL} type;
    std::vector<double> params;
    std::mt19937& _rng;
    void* _distribution {nullptr};

    Distribution(std::mt19937& rng) : _rng(rng) {}
    ~Distribution() {
        cleanUp();
    }

    void configure(Type type, const std::vector<double>& params) {
        cleanUp();
        this->type = type;
        this->params = params;
        switch (type) {
        case CONSTANT:
            _distribution = nullptr;
            break;
        case UNIFORM:
            _distribution = new std::uniform_real_distribution<double>(params[0], params[1]);
            break;
        case EXPONENTIAL:
            _distribution = new std::exponential_distribution<double>(params[0]);
            break;
        case NORMAL:
            _distribution = new std::normal_distribution<double>(params[0], params[1]);
            break;
        }
    }

    double sample() {
        switch (type) {
        case CONSTANT:
            return params[0];
        case UNIFORM:
            return (*(std::uniform_real_distribution<double>*)_distribution)(_rng);
        case EXPONENTIAL:
            return (*(std::exponential_distribution<double>*)_distribution)(_rng);
        case NORMAL:
            double result = (*(std::normal_distribution<double>*)_distribution)(_rng);
            if (params.size() > 2) result = std::max(result, params[2]);
            if (params.size() > 3) result = std::min(result, params[3]);
            return result;
        }
        return 0;
    }

    void cleanUp() {
        if (_distribution == nullptr) return;
        switch (type) {
        case UNIFORM:
            delete (std::uniform_real_distribution<double>*) _distribution;
            break;
        case EXPONENTIAL:
            delete (std::exponential_distribution<double>*) _distribution;
            break;
        case NORMAL:
            delete (std::normal_distribution<double>*) _distribution;
            break;
        }
        _distribution = nullptr;
    }
};


#pragma once

#include <vector>
#include <random>
#include <fstream>
#include <iostream>

#include "util/json.hpp"

class ClientTemplate {

private:
    struct Distribution {
        enum Type {CONSTANT, UNIFORM, EXPONENTIAL, NORMAL} type;
        std::vector<double> params;
        std::mt19937& _rng;
        void* _distribution;

        Distribution(std::mt19937& rng) : _rng(rng) {}

        void configure(Type type, const std::vector<double>& params) {
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
        }
    };

    std::mt19937 _rng;
    Distribution _dist_priority;
    Distribution _dist_maxdemand;
    Distribution _dist_wallclock_limit;
    Distribution _dist_arrival;
    Distribution _dist_burst_size;
    bool _valid = false;

    double _last_arrival;
    int _remaining_jobs_from_burst = 0;

public:
    ClientTemplate(int seed, const std::string& clientJsonFilename) :
            _rng(seed),
            _dist_priority(_rng),
            _dist_maxdemand(_rng),
            _dist_wallclock_limit(_rng),
            _dist_arrival(_rng),
            _dist_burst_size(_rng) {

        if (clientJsonFilename.empty()) return;

        nlohmann::json clientJson;
        try {
            std::ifstream i(clientJsonFilename);
            i >> clientJson;
        } catch (const nlohmann::detail::parse_error& e) {
            std::cout << "[ERROR] Parse error on job template file:" << e.what() << std::endl;
            abort();
        }

        parseDistribution(clientJson["priority"], &_dist_priority);
        parseDistribution(clientJson["maxdemand"], &_dist_maxdemand);
        parseDistribution(clientJson["wallclock-limit"], &_dist_wallclock_limit);
        parseDistribution(clientJson["arrival"], &_dist_arrival);
        parseDistribution(clientJson["burstsize"], &_dist_burst_size);
        _valid = true;
    }

    bool valid() const {return _valid;}

    double getNextPriority() {
        return _dist_priority.sample();
    }
    int getNextMaxDemand() {
        return (int)_dist_maxdemand.sample();
    }
    int getNextWallclockLimit() {
        return (int)_dist_wallclock_limit.sample();
    }
    double getNextArrival() {
        if (_dist_arrival.type == Distribution::CONSTANT && _dist_arrival.params[0] == 0) {
            return 0;
        }
        while (_remaining_jobs_from_burst == 0) {
            _remaining_jobs_from_burst = (int)std::max(1.0, std::ceil(_dist_burst_size.sample()));
            _last_arrival += _dist_arrival.sample();
        }
        _remaining_jobs_from_burst--;
        return _last_arrival;
    }

private:

    void parseDistribution(nlohmann::json& json, Distribution* dist) {
        std::vector<double> params = json["params"].get<std::vector<double>>();
        Distribution::Type type;
        if (json["type"] == "constant") type = Distribution::CONSTANT;
        else if (json["type"] == "uniform") type = Distribution::UNIFORM;
        else if (json["type"] == "exponential") type = Distribution::EXPONENTIAL;
        else if (json["type"] == "normal") type = Distribution::NORMAL;
        else {
            std::cout << "[ERROR] \"" << json["type"] << "\" is not a valid distribution type!" << std::endl;
            abort();
        }
        dist->configure(type, params);
    }

};


#pragma once

#include <vector>
#include <random>
#include <fstream>
#include <iostream>

#include "util/json.hpp"
#include "util/distribution.hpp"

class ClientTemplate {

private:
    std::mt19937 _rng;
    RandomDistribution _dist_priority;
    RandomDistribution _dist_maxdemand;
    RandomDistribution _dist_wallclock_limit;
    RandomDistribution _dist_arrival;
    RandomDistribution _dist_burst_size;
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

        RandomDistribution::parse(clientJson["priority"], &_dist_priority);
        RandomDistribution::parse(clientJson["maxdemand"], &_dist_maxdemand);
        RandomDistribution::parse(clientJson["wallclock-limit"], &_dist_wallclock_limit);
        RandomDistribution::parse(clientJson["arrival"], &_dist_arrival);
        RandomDistribution::parse(clientJson["burstsize"], &_dist_burst_size);
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
        if (_dist_arrival.type == RandomDistribution::CONSTANT && _dist_arrival.params[0] == 0) {
            return 0;
        }
        while (_remaining_jobs_from_burst == 0) {
            _remaining_jobs_from_burst = (int)std::max(1.0, std::ceil(_dist_burst_size.sample()));
            _last_arrival += _dist_arrival.sample();
        }
        _remaining_jobs_from_burst--;
        return _last_arrival;
    }
};

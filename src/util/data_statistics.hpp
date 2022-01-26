
#pragma once

#include <list>
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>

#include "util/logger.hpp"

class DataStatistics {

private:
    std::list<std::vector<float>> _data;
    size_t _num = 0;
    float _min = 0;
    float _max = 0;
    float _median = 0;
    double _mean = 0;
    std::vector<float> _percentiles;
    std::vector<float> _sorted_data;

public:
    DataStatistics(std::vector<float>&& data) {
        _data.push_back(std::move(data));
    }
    DataStatistics(std::list<std::vector<float>>&& data) : _data(std::move(data)) {}

    void computeStats() {

        size_t totalSize = 0;
        for (auto& vec : _data) totalSize += vec.size();
        if (totalSize == 0) return;

        _num = totalSize;
        _sorted_data.reserve(totalSize);
        for (auto& vec : _data) _sorted_data.insert(_sorted_data.end(), vec.begin(), vec.end());

        std::sort(_sorted_data.begin(), _sorted_data.end());

        _min = _sorted_data.front();
        _max = _sorted_data.back();
        _median = _sorted_data[_sorted_data.size()/2];
        for (int perc = 0; perc <= 100; perc++) {
            size_t idx = std::min(_sorted_data.size()-1, (size_t)std::round(perc * _sorted_data.size() / 100.f));
            _percentiles.push_back(_sorted_data[idx]);
        }
        _mean = 0;
        for (float f : _sorted_data) _mean += f;
        _mean /= _sorted_data.size();
    }
    
    size_t num() const {return _num;}
    float min() const {return _min;}
    float max() const {return _max;}
    float median() const {return _median;}
    double mean() const {return _mean;}
    const std::vector<float>& percentiles() const {return _percentiles;}
    const std::vector<float>& sortedData() const {return _sorted_data;}

    void logFullDataIntoFile(const std::string& logfileSuffix) const {
        auto statsLogger = Logger::getMainInstance().copy("", logfileSuffix);
        statsLogger.setQuiet();
        for (float f : sortedData()) LOGGER_OMIT_PREFIX(statsLogger, V3_VERB, "%.6f\n", f);
    }
};

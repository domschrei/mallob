
#pragma once

#include <string>
#include <vector>

class StringUtils {

public:
    template<typename T>
    static std::string getSummary(const std::vector<T>& collection, int maxElems = 10) {
        return getSummary(collection.data(), collection.size(), maxElems);
    }
    template<typename T>
    static std::string getSummary(const T* data, size_t size, int maxElems = 10) {
        std::string summary;
        const int halfwayPoint = maxElems/2;
        for (int i = 0; i < size; i++) {
            if (i >= halfwayPoint && i+(maxElems-halfwayPoint) < size) {
                if (i == halfwayPoint) summary += "... ";
                i = std::max(i, (int) size - (maxElems-halfwayPoint) - 1);
                continue;
            }
            summary += std::to_string(data[i]) + " ";
        }
        return summary;
    }
};

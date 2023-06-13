
#pragma once

#include <vector>
#include <string>

class StrUtil {

public:
    template <typename T>
    static std::string vecToStr(const std::vector<T>& vec) {
        if (vec.empty()) return "[]";
        std::string out = "[";
        for (auto& elem : vec) out += std::to_string(elem) + " ";
        return out.substr(0, out.size()-1) + "]";
    }

};

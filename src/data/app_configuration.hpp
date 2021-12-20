
#pragma once

#include <string>
#include <map>
#include <sstream>
#include "util/assert.hpp"

/*
Used to store application-specific configuration options.
Must be of constant size and have primitive members only.
Introduce new fields here and properly set them in JsonInterface::handle.
*/
struct AppConfiguration {

std::map<std::string, std::string> map;

int getSerializedSize() const {
    int size = 0;
    for (auto& [key, val] : map) {
        // "-key=value;"
        size += 1 + key.size() + 1 + val.size() + 1;
    }
    return size;
}

std::string serialize() const {
    std::string out = "";
    for (auto& [key, val] : map) {
        out += "-" + key + "=" + val + ";";
    }
    assert(out.size() == getSerializedSize());
    return out;
}

void deserialize(const std::string& packed) {
    map.clear();

    std::stringstream s_stream(packed);
    std::string substr;
    getline(s_stream, substr, ';'); 
    
    while (!substr.empty()) {

        assert(substr[0] == '-');
        substr = substr.substr(1);
        
        std::string key, val;
        {
            std::stringstream ss_sub(substr);
            getline(ss_sub, key, '=');
            getline(ss_sub, val, ';');
        }
        map[key] = val;

        getline(s_stream, substr, ';');     
    }
}

};

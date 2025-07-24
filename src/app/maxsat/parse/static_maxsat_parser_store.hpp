
#pragma once

#include "util/tsl/robin_map.h"
#include "parserinterface.hpp"

using ParserPtr = std::shared_ptr<maxPreprocessor::ParserInterface>;

class StaticMaxSatParserStore {

private:
    static tsl::robin_map<int, ParserPtr> _parsers;

public:
    static ParserPtr get(int id) {
        if (!_parsers.count(id)) {
            _parsers[id].reset(new maxPreprocessor::ParserInterface());
        }
        return _parsers[id];
    }
    static void erase(int id) {
        _parsers.erase(id);
    }
};

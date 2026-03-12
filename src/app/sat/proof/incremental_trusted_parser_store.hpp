
#pragma once

#include "app/sat/proof/trusted_parser_process_adapter.hpp"
#include "robin_map.h"
#include "util/sys/threading.hpp"

struct IncrementalTrustedParserStore {
    static Mutex mtxMap;
    static tsl::robin_map<int, std::shared_ptr<TrustedParserProcessAdapter>> map;
};

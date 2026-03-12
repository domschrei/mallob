
#include "incremental_trusted_parser_store.hpp"

Mutex IncrementalTrustedParserStore::mtxMap;
tsl::robin_map<int, std::shared_ptr<TrustedParserProcessAdapter>> IncrementalTrustedParserStore::map;

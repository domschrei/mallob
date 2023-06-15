
#include "qbf_context_store.hpp"

Mutex QbfContextStore::_mtx_map;
tsl::robin_map<int, std::pair<std::unique_ptr<Mutex>, std::unique_ptr<QbfContext>>> QbfContextStore::_map;

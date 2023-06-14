
#include "qbf_context_store.hpp"

Mutex QbfContextStore::_mtx_map;
tsl::robin_map<int, std::unique_ptr<QbfContext>> QbfContextStore::_map;

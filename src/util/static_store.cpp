
#include "static_store.hpp"

// Specialization for std::vector<int>
template <>
Mutex StaticStore<std::vector<int>>::_mtx_map;
template <>
std::map<std::string, std::vector<int>> StaticStore<std::vector<int>>::_map;


#include <cstring>

template<typename T> 
T Serializable::get(const std::vector<uint8_t>& packed) {
    if constexpr (std::is_base_of<Serializable, T>()) {
        return T().deserialize(packed);
    } else {
        T elem;
        memcpy(&elem, packed.data(), sizeof(T));
        return elem;
    }
}

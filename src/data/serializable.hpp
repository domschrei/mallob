
#ifndef DOMPASCH_MALLOB_SERIALIZABLE_HPP
#define DOMPASCH_MALLOB_SERIALIZABLE_HPP

#include <vector>
#include <cstdint>
#include <cstring>

/*
Generic interface for serializing and deserializing arbitrary data, mainly
for the purpose of seamlessly sending it around via MPI.

To serialize a Serializable object, call the serialize() method returning
a shared_ptr to a byte vector.

To deserialize a byte vector, call Serializable::get<T>(byteVector); thereby,
T must be a fitting Serializable object or primitive data type.
Alternatively, you can call deserialize(byteVector) on an "empty" Serializable object. 
*/
class Serializable {

public:
    virtual std::vector<uint8_t> serialize() const = 0;
    virtual Serializable& deserialize(const std::vector<uint8_t>& packed) = 0;
    
    template<typename T>
    static T get(const std::vector<uint8_t>& packed);
};

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

#endif

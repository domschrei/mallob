
#ifndef DOMPASCH_CUCKOO_REBALANCER_SERIALIZABLE
#define DOMPASCH_CUCKOO_REBALANCER_SERIALIZABLE

#include <vector>
#include <cstdint>
#include <memory>

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
    virtual std::shared_ptr<std::vector<uint8_t>> serialize() const = 0;
    virtual Serializable& deserialize(const std::vector<uint8_t>& packed) = 0;
    
    template<typename T>
    static T get(const std::vector<uint8_t>& packed);
};

#include "serializable_impl.hpp"

#endif

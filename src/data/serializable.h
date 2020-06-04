
#ifndef DOMPASCH_CUCKOO_REBALANCER_SERIALIZABLE
#define DOMPASCH_CUCKOO_REBALANCER_SERIALIZABLE

#include <vector>
#include <cstdint>
#include <memory>

class Serializable {

public:
    virtual std::shared_ptr<std::vector<uint8_t>> serialize() const = 0;
    virtual Serializable& deserialize(const std::vector<uint8_t>& packed) = 0;
    
    template<typename T>
    static T get(const std::vector<uint8_t>& packed);
};

#include "serializable_impl.h"

#endif


#ifndef DOMPASCH_CUCKOO_REBALANCER_SERIALIZABLE
#define DOMPASCH_CUCKOO_REBALANCER_SERIALIZABLE

#include <vector>
#include <cstdint>

class Serializable {

public:
    virtual std::vector<uint8_t> serialize() const = 0;
    virtual void deserialize(const std::vector<uint8_t>& packed) = 0;
};

#endif

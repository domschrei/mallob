
#ifndef DOMPASCH_CUCKOO_REBALANCER_SERIALIZABLE
#define DOMPASCH_CUCKOO_REBALANCER_SERIALIZABLE

#include <vector>

class Serializable {

public:
    virtual std::vector<int> serialize() const = 0;
    virtual void deserialize(const std::vector<int>& packed) = 0;
};

#endif

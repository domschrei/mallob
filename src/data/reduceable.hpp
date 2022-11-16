
#ifndef DOMPASCH_MALLOB_REDUCEABLE_HPP
#define DOMPASCH_MALLOB_REDUCEABLE_HPP

#include <memory>

#include "serializable.hpp"

class Reduceable : public Serializable {

public:
    virtual ~Reduceable() = default;

    virtual std::vector<uint8_t> serialize() const override = 0;
    virtual Reduceable& deserialize(const std::vector<uint8_t>& packed) override = 0;

    // Return whether the instance represents the empty / neutral element.
    virtual bool isEmpty() const = 0;
    // Associative operation [ (axb)xc = ax(bxc) ] where "other" represents
    // the "right" element and this instance represents the "left" element.
    // The operation does not need to be commutative [ axb = bxa ].
    virtual void aggregate(const Reduceable& other) = 0;
};

#endif

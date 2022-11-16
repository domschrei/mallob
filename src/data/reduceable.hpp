
#ifndef DOMPASCH_MALLOB_REDUCEABLE_HPP
#define DOMPASCH_MALLOB_REDUCEABLE_HPP

#include <memory>

#include "serializable.hpp"

class Reduceable : public Serializable {

public:
    virtual ~Reduceable() = default;

    virtual std::vector<uint8_t> serialize() const override = 0;
    virtual Reduceable& deserialize(const std::vector<uint8_t>& packed) override = 0;
    virtual void aggregate(const Reduceable& other) = 0;
    virtual bool isEmpty() const = 0;
};

#endif

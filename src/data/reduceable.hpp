
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

// Integer wrapper with addition as an aggregation operation.
struct ReduceableInt : public Reduceable {
    int content {0};
    ReduceableInt() = default;
    ReduceableInt(int content) : content(content) {}
    virtual std::vector<uint8_t> serialize() const override {
        std::vector<uint8_t> result(sizeof(int));
        memcpy(result.data(), &content, sizeof(int));
        return result;
    }
    virtual ReduceableInt& deserialize(const std::vector<uint8_t>& packed) override {
        memcpy(&content, packed.data(), sizeof(int));
        return *this;
    }
    virtual void aggregate(const Reduceable& other) {
        ReduceableInt* otherInt = (ReduceableInt*) &other;
        content += otherInt->content;
    }
    virtual bool isEmpty() const {
        return content == 0;
    }
    bool operator==(const ReduceableInt& other) const {
        return content == other.content;
    }
    bool operator!=(const ReduceableInt& other) const {
        return !(*this == other);
    }
};

#endif

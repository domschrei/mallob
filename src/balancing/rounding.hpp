

#ifndef DOMPASCH_BALANCER_ROUNDING_H
#define DOMPASCH_BALANCER_ROUNDING_H

#include <map>
#include <set>
#include <vector>
#include <cstring>

#include "data/reduceable.hpp"

struct SortedDoubleSequence : public Reduceable {
    std::vector<double> data;

SortedDoubleSequence() : data() {}

void add(double x) {
    size_t i = 0;
    while (i < data.size() && data[i] < x) i++;
    // Only insert unique elements
    if (i >= data.size() || x != data[i])
        data.insert(data.begin() + i, x);
}

int size() const {return data.size();}

const double& operator[](int i) const {return data[i];}

void merge(const Reduceable& other) override {
    const SortedDoubleSequence& otherSet = (SortedDoubleSequence&) other;
    size_t i = 0, j = 0;
    std::vector<double> newData;
    for (size_t x = 0; x < data.size()+otherSet.data.size(); x++) {
        
        // Identify correct element to insert next
        double newElem;
        if (i < data.size() && (j >= otherSet.data.size() || data[i] <= otherSet.data[j]))
            newElem = data[i++];
        else
            newElem = otherSet.data[j++];
        assert((newElem > 0 && newElem < 1.0) || Console::fail("%.3f is an invalid remainder to reduce!", newElem));
        
        // Only insert unique elements
        if (newData.empty() || newElem != newData.back()) {
            newData.push_back(newElem);
        } 
    }
    this->data = newData;
}

bool isEmpty() const override {
    return data.empty();
}

std::vector<uint8_t> serialize() const override {
    int size = this->data.size()*sizeof(double);
    std::vector<uint8_t> data(size);
    memcpy(data.data(), this->data.data(), size);
    return data;
}

SortedDoubleSequence& deserialize(const std::vector<uint8_t>& packed) override {
    if (packed.size() <= 1) {
        // Empty / stub object
        return *this;
    }
    assert(packed.size() % sizeof(double) == 0 || Console::fail("%i not a multiple of %i!", packed.size(), sizeof(double)));
    int size = packed.size() / sizeof(double);
    this->data.clear(); this->data.resize(size);
    memcpy(this->data.data(), packed.data(), packed.size());
    return *this;
}

std::unique_ptr<Reduceable> getDeserialized(const std::vector<uint8_t>& packed) const override {
    std::unique_ptr<Reduceable> out(new SortedDoubleSequence());
    out->deserialize(packed);
    return out;
}
};

namespace Rounding {

    robin_hood::unordered_map<int, int> getRoundedAssignments(int remainderIdx, int& sum, 
        const SortedDoubleSequence& remainders, const robin_hood::unordered_map<int, double>& assignments);
    float penalty(float utilization, float loadFactor);
}

#endif
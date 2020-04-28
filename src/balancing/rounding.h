

#ifndef DOMPASCH_BALANCER_ROUNDING_H
#define DOMPASCH_BALANCER_ROUNDING_H

#include <map>
#include <set>
#include <vector>
#include <cstring>

#include "data/reduceable.h"

struct SortedDoubleSequence : public Reduceable {
    std::vector<double> data;

SortedDoubleSequence() : data() {}

void add(double x) {
    int i = 0;
    while (i < data.size() && data[i] < x) i++;
    // Only insert unique elements
    if (i >= data.size() || x != data[i])
        data.insert(data.begin() + i, x);
}

int size() const {return data.size();}

const double& operator[](int i) const {return data[i];}

void merge(const Reduceable& other) override {
    const SortedDoubleSequence& otherSet = (SortedDoubleSequence&) other;
    int i = 0, j = 0;
    std::vector<double> newData;
    for (int x = 0; x < data.size()+otherSet.data.size(); x++) {
        
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

bool isEmpty() override {
    return data.empty();
}

std::shared_ptr<std::vector<uint8_t>> serialize() const override {
    int size = this->data.size()*sizeof(double);
    std::shared_ptr<std::vector<uint8_t>> data = std::make_shared<std::vector<uint8_t>>(size);
    memcpy(data->data(), this->data.data(), size);
    return data;
}

void deserialize(const std::vector<uint8_t>& packed) override {
    if (packed.size() <= 1) {
        // Empty / stub object
        return;
    }
    assert(packed.size() % sizeof(double) == 0 || Console::fail("%i not a multiple of %i!", packed.size(), sizeof(double)));
    int size = packed.size() / sizeof(double);
    this->data.clear(); this->data.resize(size);
    memcpy(this->data.data(), packed.data(), packed.size());
}

std::unique_ptr<Reduceable> getDeserialized(const std::vector<uint8_t>& packed) const override {
    std::unique_ptr<Reduceable> out(new SortedDoubleSequence());
    out->deserialize(packed);
    return out;
}
};

namespace Rounding {

    std::map<int, int> getRoundedAssignments(int remainderIdx, int& sum, 
        const SortedDoubleSequence& remainders, const std::map<int, double>& assignments);
    float penalty(float utilization, float loadFactor);
}

#endif
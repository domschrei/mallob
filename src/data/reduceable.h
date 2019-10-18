
#ifndef DOMPASCH_BALANCER_REDUCEABLE_H
#define DOMPASCH_BALANCER_REDUCEABLE_H

#include <memory>

#include "util/mpi.h"
#include "serializable.h"

class Reduceable : public Serializable {
public:
    virtual std::vector<int> serialize() const override = 0;
    virtual void deserialize(const std::vector<int>& packed) override = 0;
    virtual void merge(const Reduceable& other) = 0;
    virtual std::unique_ptr<Reduceable> getDeserialized(const std::vector<int>& packed) const = 0;
    virtual bool isEmpty() = 0;

    std::set<int> allReduce(MPI_Comm& comm);
    std::set<int> reduceToRankZero(MPI_Comm& comm);
    void broadcastFromRankZero(MPI_Comm& comm, std::set<int> excludedRanks = std::set<int>());
};

#endif
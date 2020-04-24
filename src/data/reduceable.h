
#ifndef DOMPASCH_BALANCER_REDUCEABLE_H
#define DOMPASCH_BALANCER_REDUCEABLE_H

#include <memory>

#include "util/mympi.h"
#include "serializable.h"

class Reduceable : public Serializable {

protected:
    MPI_Comm _comm = NULL;
    int _my_rank = -1;
    std::set<int> _excluded_ranks;
    int _power;
    int _highest_power;

public:
    virtual ~Reduceable() = default;

    virtual std::shared_ptr<std::vector<uint8_t>> serialize() const override = 0;
    virtual void deserialize(const std::vector<uint8_t>& packed) override = 0;
    virtual void merge(const Reduceable& other) = 0;
    virtual std::unique_ptr<Reduceable> getDeserialized(const std::vector<uint8_t>& packed) const = 0;
    virtual bool isEmpty() = 0;

    std::set<int> allReduce(MPI_Comm& comm);
    std::set<int> reduceToRankZero(MPI_Comm& comm);
    void broadcastFromRankZero(MPI_Comm& comm, std::set<int> excludedRanks = std::set<int>());

    bool startReduction(MPI_Comm& comm, std::set<int> excludedRanks = std::set<int>());
    bool advanceReduction(MessageHandlePtr handle);
    std::set<int>& getExcludedRanks() {return _excluded_ranks;}

    bool startBroadcast(MPI_Comm& comm, std::set<int>& excludedRanks);
    bool advanceBroadcast(MessageHandlePtr handle);
};

#endif
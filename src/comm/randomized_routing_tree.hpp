
#pragma once

#include "util/params.hpp"
#include "mympi.hpp"
#include "util/permutation.hpp"
#include "util/random.hpp"

class RandomizedRoutingTree {

private:
    Parameters& _params;
    MPI_Comm& _comm;

    int _world_rank;
    int _num_workers;

    std::vector<int> _hop_destinations;
    std::vector<int> _neighbor_towards_rank;

    int _epoch {0};

public:
    RandomizedRoutingTree(Parameters& params, MPI_Comm& comm) : _params(params), _comm(comm) {
        _world_rank = MyMpi::rank(_comm);
        _num_workers = MyMpi::size(_comm);
        init();
    }

    void init() {

        // Pick fixed number k of bounce destinations
        int numBounceAlternatives = _params.numBounceAlternatives();
        _num_workers = MyMpi::size(_comm);
        
        // Check validity of num bounce alternatives
        if (2*numBounceAlternatives > _num_workers) {
            numBounceAlternatives = std::max(1, _num_workers / 2);
            LOG(V1_WARN, "[WARN] Num bounce alternatives must be at most half the number of workers!\n");
            LOG(V1_WARN, "[WARN] Falling back to safe value r=%i.\n", numBounceAlternatives);
        }  

        // Create graph, get outgoing edges from this node
        auto permutations = AdjustablePermutation::getPermutations(_num_workers, numBounceAlternatives);
        _hop_destinations = AdjustablePermutation::createExpanderGraph(permutations, _world_rank);
        
        // Output found bounce alternatives
        std::string info = "";
        for (size_t i = 0; i < _hop_destinations.size(); i++) {
            info += std::to_string(_hop_destinations[i]) + " ";
        }
        LOG(V3_VERB, "My bounce alternatives: %s\n", info.c_str());
        assert((int)_hop_destinations.size() == numBounceAlternatives);

        _neighbor_towards_rank = AdjustablePermutation::getBestOutgoingEdgeForEachNode(permutations, _world_rank);
    }

    void setEpoch(int epoch) {
        _epoch = epoch;
    }

    int getCurrentRoot() const {
        assert(_num_workers > 0);
        return robin_hood::hash<int>()(_epoch) % _num_workers;
    }

    int getCurrentParent() const  {
        return _neighbor_towards_rank.at(getCurrentRoot());
    }

    int getRandomNeighbor() const {
        return Random::choice(_hop_destinations);
    }

    int getNumNeighbors() const {
        return _hop_destinations.size();
    }

    const std::vector<int>& getNeighbors() const {
        return _hop_destinations;
    }

    int getNumWorkers() const {
        return MyMpi::size(_comm);
    }
};

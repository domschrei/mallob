
#ifndef DOMPASCH_BALANCER_INTERFACE_H
#define DOMPASCH_BALANCER_INTERFACE_H

#include "util/robin_hood.hpp"
#include "app/job.hpp"
#include "comm/mympi.hpp"
#include "util/params.hpp"

class Balancer {

public:
    Balancer(MPI_Comm& comm, Parameters& params) :
    _comm(comm), _params(params), _load_factor(params.loadFactor()), _balancing(false) {}
    virtual ~Balancer() {}

    virtual void forget(int jobId);

    // Asynchronous rebalancing

    /**
     * Returns whether balancing is currently being done.
     */
    bool isBalancing() {return _balancing;};

    /**
     * Do first part of balancing procedure, until finished or until some synchronization is necessary.
     * Returns true if finished.
     */
    virtual bool beginBalancing(robin_hood::unordered_map<int, Job*>& jobs) = 0;
    /**
     * True if any necessary synchronization stage is finished such that the balancing can continue.
     */
    virtual bool canContinueBalancing() = 0;
    /**
     * Continues balancing until finished or until some synchronization is necessary.
     * Returns true if finished.
     */
    virtual bool continueBalancing() = 0;
    /**
     * Processes a message adressed to the balancing procedure.
     * Returns true if the balancing finished.
     */
    virtual bool continueBalancing(MessageHandle& handle) = 0;
    /**
     * If balancing finished, returns the result.
     */
    virtual robin_hood::unordered_map<int, int> getBalancingResult() = 0;

    virtual size_t getGlobalEpoch() const = 0;

protected:
    float allReduce(float contribution) const;
    void iAllReduce(float contribution);
    float reduce(float contribution, int rootRank) const;
    void iReduce(float contribution, int rootRank);

    int getDemand(const Job& job);

protected:
    MPI_Comm& _comm;
    Parameters& _params;
    float _load_factor;
    bool _balancing;
    int _balancing_epoch = 0;

    robin_hood::unordered_map<int, Job*> _jobs_being_balanced;

    float _reduce_contrib;
    MPI_Request _reduce_request;
    float _reduce_result;
};

#endif


#ifndef DOMPASCH_BALANCER_INTERFACE_H
#define DOMPASCH_BALANCER_INTERFACE_H

#include <map>

#include "data/job.h"
#include "data/statistics.h"
#include "util/mympi.h"
#include "util/params.h"

class Balancer {

public:
    Balancer(MPI_Comm& comm, Parameters& params, Statistics& stats) :
    _comm(comm), _params(params), _stats(stats), _load_factor(params.getFloatParam("l")), _balancing(false) {}

    int getVolume(int jobId);
    bool hasVolume(int jobId);
    void updateVolume(int jobId, int volume);


    // Asynchronous rebalancing

    /**
     * Returns whether balancing is currently being done.
     */
    bool isBalancing() {return _balancing;};

    /**
     * Do first part of balancing procedure, until finished or until some synchronization is necessary.
     * Returns true if finished.
     */
    virtual bool beginBalancing(std::map<int, Job*>& jobs) = 0;
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
    virtual bool continueBalancing(MessageHandlePtr handle) = 0;
    /**
     * If balancing finished, returns the result.
     */
    virtual std::map<int, int> getBalancingResult() = 0;

protected:
    float allReduce(float contribution) const;
    void iAllReduce(float contribution);
    float reduce(float contribution, int rootRank) const;
    void iReduce(float contribution, int rootRank);
    int getDemand(const Job& job);

protected:
    MPI_Comm& _comm;
    Parameters& _params;
    Statistics& _stats;
    float _load_factor;
    bool _balancing;
    int _balancing_epoch = 0;

    std::map<int, Job*> _jobs_being_balanced;
    std::map<int, int> _volumes;
    std::map<int, float> _priorities;
    std::map<int, int> _demands;
    std::map<int, double> _temperatures;

    float _reduce_contrib;
    MPI_Request _reduce_request;
    float _reduce_result;
};

#endif

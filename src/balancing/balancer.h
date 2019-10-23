
#ifndef DOMPASCH_BALANCER_INTERFACE_H
#define DOMPASCH_BALANCER_INTERFACE_H

#include <map>

#include "data/job.h"
#include "data/statistics.h"
#include "util/mpi.h"
#include "util/params.h"

class Balancer {

public:
    Balancer(MPI_Comm& comm, Parameters& params, Statistics& stats) : 
    comm(comm), params(params), stats(stats), loadFactor(params.getFloatParam("l")), balancing(false) {}
    
    virtual std::map<int, int> balance(std::map<int, Job*>& jobs) = 0;
    int getVolume(int jobId);
    void updateVolume(int jobId, int volume);



    // Asynchronous rebalancing

    /**
     * Returns whether balancing is currently being done. 
     */
    bool isBalancing() {return balancing;};

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
    virtual bool handleMessage(MessageHandlePtr handle) = 0;
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
    MPI_Comm& comm;
    Parameters& params;
    Statistics& stats;
    float loadFactor;
    bool balancing;

    std::map<int, Job*> jobsBeingBalanced; 
    std::map<int, int> volumes;
    std::map<int, float> priorities;
    std::map<int, int> demands;

    MPI_Request reduceRequest;
    float reduceResult;
};

#endif
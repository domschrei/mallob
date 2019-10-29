
#ifndef DOMPASCH_BALANCER_THERMODYNAMIC_H
#define DOMPASCH_BALANCER_THERMODYNAMIC_H

#include "balancing/balancer.h"

class ThermodynamicBalancer : public Balancer {

public:
    ThermodynamicBalancer(MPI_Comm& comm, Parameters& params, Statistics& stats) : Balancer(comm, params, stats) {
        
    }
    std::map<int, int> balance(std::map<int, Job*>& jobs) override;

private:
    float calculatePressure(const std::vector<JobDescription*>& involvedJobs, float volume);

    float getTemperature(int jobId) {
        if (_temperatures.count(jobId)) {
            _temperatures[jobId] = 100.0;
        }
        return _temperatures[jobId];
    }
    void coolDown(int jobId) {
        float room = 20.0; 
        float decay = _params.getFloatParam("td");
        float previous = getTemperature(jobId);
        _temperatures[jobId] = previous - decay * (previous - room);
    }

    std::map<int, float> _temperatures;
};

#endif

#pragma once

#include <set>
#include <functional>

#include "data/job_transfer.hpp"
#include "comm/mympi.hpp"

class JobDatabase; // forward declaration

class RequestMatcher {

protected:
    JobDatabase* _job_db = nullptr;
    std::function<void(const JobRequest&, int)> _local_request_callback;
    int _num_workers;    
    int _epoch = -1;
    bool _status_dirty = true;

public:
    RequestMatcher(JobDatabase& jobDb, MPI_Comm workersComm, std::function<void(const JobRequest&, int)> localRequestCallback) : 
        _job_db(&jobDb), _local_request_callback(localRequestCallback), _num_workers(MyMpi::size(workersComm)) {}

    virtual void handle(MessageHandle& handle) = 0;
    virtual void advance(int epoch) = 0;
    virtual void addJobRequest(JobRequest& request) = 0;

    virtual void setStatusDirty() {_status_dirty = true;}

protected:
    bool isIdle();
};

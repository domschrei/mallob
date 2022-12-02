
#pragma once

#include <set>
#include <functional>

#include "data/job_transfer.hpp"
#include "comm/mympi.hpp"
#include "core/job_registry.hpp"

class SchedulingManager; // forward declaration

class RequestMatcher {

protected:
    JobRegistry* _job_registry = nullptr;
    std::function<void(const JobRequest&, int)> _local_request_callback;
    int _num_workers;    
    int _epoch = -1;
    bool _status_dirty = true;

public:
    RequestMatcher(JobRegistry& jobRegistry, MPI_Comm workersComm, std::function<void(const JobRequest&, int)> localRequestCallback) : 
        _job_registry(&jobRegistry), _local_request_callback(localRequestCallback), _num_workers(MyMpi::size(workersComm)) {}

    virtual void handle(MessageHandle& handle) = 0;
    virtual void advance(int epoch) = 0;
    virtual void addJobRequest(JobRequest& request) = 0;

    enum StatusDirtyReason {
        DISCARD_REQUEST, REJECT_REQUEST, STOP_WAIT_FOR_REACTIVATION, COMMIT_JOB, UNCOMMIT_JOB_LEAVING, BECOME_IDLE
    };
    virtual void setStatusDirty(StatusDirtyReason reason) {_status_dirty = true;}

protected:
    bool isIdle();
};

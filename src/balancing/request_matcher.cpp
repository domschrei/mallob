
#include "request_matcher.hpp"

#include "core/scheduling_manager.hpp"

bool RequestMatcher::isIdle() {
    return !_job_registry->isBusyOrCommitted() 
        && !_job_registry->hasInactiveJobsWaitingForReactivation() 
        && !_job_registry->hasDormantRoot();
}

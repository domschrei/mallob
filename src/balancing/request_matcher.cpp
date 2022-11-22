
#include "request_matcher.hpp"

#include "data/job_database.hpp"

bool RequestMatcher::isIdle() {
    return !_job_db->isBusyOrCommitted() && !_job_db->hasInactiveJobsWaitingForReactivation() && !_job_db->hasDormantRoot();
}

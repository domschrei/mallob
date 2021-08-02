
#ifndef DOMPASCH_MALLOB_JOB_STATE_H
#define DOMPASCH_MALLOB_JOB_STATE_H

/**
 * Internal state of the job's image on this node.
 */
enum JobState {
    INACTIVE, ACTIVE, SUSPENDED, PAST
};
static const char * JOB_STATE_STRINGS[] = { "inactive", "active", "suspended", "past" };

#endif
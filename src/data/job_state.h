
#ifndef DOMPASCH_MALLOB_JOB_STATE_H
#define DOMPASCH_MALLOB_JOB_STATE_H

/**
 * Internal state of the job's image on this node.
 */
enum JobState {
    /**
     * The job is known for some reason (e.g. a failed commitment),
     * but no job description is known and the job has never been launched.
     */
    NONE,
    /**
     * The job description is known, but the job has never been launched.
     */
    STORED,
    /**
     * A commitment has been made to compute on the job as the node of a certain index,
     * but the job description is not necessarily known yet.
     * The job may also have been started before.
     */
    COMMITTED,
    /**
     * The job is currently being initialized (by a separate thread).
     */
    INITIALIZING_TO_ACTIVE,
    INITIALIZING_TO_SUSPENDED,
    INITIALIZING_TO_PAST,
    INITIALIZING_TO_COMMITTED,
    /**
     * There are threads actively computing on the job.
     */
    ACTIVE,
    /**
     * The threads that once computed on the job are suspended.
     * They may or may not be resumed at a later point.
     */
    SUSPENDED,
    /**
     * The job has been finished or terminated in some sense.
     */
    PAST,
    /*
     * The job has been finished and is waiting for a directive from its parent
     * and/or the external client.
     */
    STANDBY,
    /*
     * The job is being destructed in the moment and will then transition into state PAST.
     */
    FORGETTING
};
static const char * jobStateStrings[] = { "none", "stored", "committed", "initializingToActive", 
    "initializingToSuspended", "initializingToPast", "initializingToCommitted", "active", "suspended", 
    "past", "standby", "forgetting" };

#endif
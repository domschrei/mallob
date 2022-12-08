
#ifndef DOMPASCH_MALLOB_MSG_TAGS_H
#define DOMPASCH_MALLOB_MSG_TAGS_H

/*
A warmup message with dummy payload and without any information.
*/
const int MSG_WARMUP = 1;
/*
The sender wishes to receive the current volume of job j from the receiver.
Data type: 1 int (jobId)
*/
const int MSG_QUERY_VOLUME = 2;
/*
The receiver is queried to begin working as the i-th node of job j.
Data type: JobRequest
*/
const int MSG_REQUEST_NODE = 3;
/*
The sender asks the receiver to become the sender's parent for some job j
of which a corresponding child position was advertised.
Data type: JobRequest
*/
const int MSG_OFFER_ADOPTION = 4;

const int MSG_ANSWER_ADOPTION_OFFER = 5;

const int MSG_QUERY_JOB_DESCRIPTION = 6;

/*
The sender propagates a job's volume update to the receiver.
Data type: [jobId, volume, globalCommEpoch] (3 ints)
*/
const int MSG_NOTIFY_VOLUME_UPDATE = 8;
/*
The sender transfers a full job description to the receiver.
Data type: JobDescription
Warning: Length may exceed the default maximum message length.
*/
const int MSG_SEND_JOB_DESCRIPTION = 9;
/*
The sender informs the receiver that a solution was found for the job
of the specified ID.
Data type: [jobId, resultCode]
*/
const int MSG_NOTIFY_RESULT_FOUND = 10;
/*
A signal to terminate a job is propagated.
Data type: [jobId]
*/
const int MSG_NOTIFY_JOB_TERMINATING = 12;
/*
The sender informs the receiver (a client) that a job has been finished,
and also provides the size of the upcoming job result message.
Data type: [jobId, sizeOfResult]
*/
const int MSG_NOTIFY_JOB_DONE = 13;
/*
The sender (a client) acknowledges that it received the receiver's MSG_JOB_DONE
message and signals that it wishes to receive the full job result.
Data type: [jobId, sizeOfResult]
*/
const int MSG_QUERY_JOB_RESULT = 14;
/*
The sender provides a job's full result to the receiver (a client).
Data type: JobResult
Warning: Length may exceed the default maximum message length.
*/
const int MSG_SEND_JOB_RESULT = 15;
/*
The sender (a worker node) informs the receiver (the job's root node) that 
the sender is defecting to another job.
Data type: [jobId, index]
*/
const int MSG_NOTIFY_NODE_LEAVING_JOB = 16;
/* 
The sender (a client or a worker) informs the receiver (a worker) that a certain
incremental job is completed and can be cleaned up.
Data type: [jobId]
*/
const int MSG_INCREMENTAL_JOB_FINISHED = 22;
/*
The sender informs the receiver that the receiver should interrupt 
the specified job it currently computes on (leaving the possibility 
to continue computation at some later point). Possibly self message.
Data type: [jobId, index]
*/
const int MSG_INTERRUPT = 23;
/*
The sender informs the receiver that the receiver should abort, i.e., 
terminate the specified job it currently computes on. Possibly self message.
Data type: [jobId, index]
*/
const int MSG_NOTIFY_JOB_ABORTING = 24;
/*
A message that tells some node (worker or client) to immediately exit the application.
*/
const int MSG_DO_EXIT = 25;
/*
Some data is being reduced or broadcast via a custom operation.
*/
const int MSG_COLLECTIVE_OPERATION = 27;
/*
Some data is being reduced via a custom operation.
*/
const int MSG_REDUCE_DATA = 28;
/*
Some data is being broadcast via a custom operation.
*/
const int MSG_BROADCAST_DATA = 29;
/*
Tag for the job-internal, application-specific communication inside a job.
The payload should contain another job-internal message tag.
*/
const int MSG_SEND_APPLICATION_MESSAGE = 30;
/*
The receiver is queried to begin working as the i-th node of job j.
The sender hopes that the receiver still remembers job j as the 
receiver was associated with it in the past.
Data type: JobRequest
*/
const int MSG_REQUEST_NODE_ONESHOT = 32;
/*
The sender declines a job request that was directed specifically to him.
*/
const int MSG_REJECT_ONESHOT = 33;
/*
The sender notifies the receiver that the job result the receiver just sent
is obsolete and will not be needed. It does not need to be preserved.
*/
const int MSG_NOTIFY_RESULT_OBSOLETE = 34;

const int MSG_NOTIFY_NEIGHBOR_STATUS = 35;

const int MSG_NOTIFY_NEIGHBOR_IDLE_DISTANCE = 36;

const int MSG_REQUEST_WORK = 37;

const int MSG_REQUEST_IDLE_NODE_BFS = 38;
const int MSG_ANSWER_IDLE_NODE_BFS = 39;

const int MSG_NOTIFY_ASSIGNMENT_UPDATE = 40;

const int MSG_NOTIFY_CLIENT_JOB_ABORTING = 41;
const int MSG_OFFER_ADOPTION_OF_ROOT = 42;

const int MSG_SCHED_INITIALIZE_CHILD_WITH_NODES = 51; // downwards
const int MSG_SCHED_RETURN_NODES = 52; // upwards
const int MSG_SCHED_RELEASE_FROM_WAITING = 53;
const int MSG_SCHED_NODE_FREED = 54;

const int MSG_JOB_TREE_REDUCTION = 61;
const int MSG_JOB_TREE_BROADCAST = 62;

const int MSG_ASYNC_COLLECTIVE_UP = 71;
const int MSG_ASYNC_COLLECTIVE_DOWN = 72;
const int MSG_ASYNC_SPARSE_COLLECTIVE_UP = 73;
const int MSG_ASYNC_SPARSE_COLLECTIVE_DOWN = 74;

const int MSG_MATCHING_SEND_IDLE_TOKEN = 81;
const int MSG_MATCHING_SEND_REQUEST = 82;
const int MSG_MATCHING_SEND_REQUEST_OBSOLETE_NOTIFICATION = 83;
const int MSG_MATCHING_REQUEST_CANCELLED = 84;

const int MSG_OFFSET_BATCHED = 10000;

// Application message tags
const int MSG_INITIATE_CLAUSE_SHARING = 416;
const int MSG_ALLREDUCE_CLAUSES = 417;
const int MSG_ALLREDUCE_FILTER = 418;
const int MSG_AGGREGATE_RANKLIST = 419;
const int MSG_BROADCAST_RANKLIST = 420; // blaze it



#endif
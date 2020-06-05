
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
The sender asks the receiver to become the sender's parent for some job j
of which a corresponding child position was advertised.
Data type: JobRequest
*/
const int MSG_OFFER_ADOPTION = 4;
/*
The senders confirms that the receiver may become the sender's child
with respect to the job and index specified in the signature.
Data type: JobSignature
*/
const int MSG_ACCEPT_ADOPTION_OFFER = 5;
/*
The sender rejects the receiver to become the sender's child
with respect to the job and index specified in the signature.
Data type: JobRequest
*/
const int MSG_REJECT_ADOPTION_OFFER = 6;
/*
The sender acknowledges that it received the receiver's previous
MSG_ACCEPT_ADOPTION_OFFER message.
Data type: JobRequest
*/
const int MSG_CONFIRM_ADOPTION = 7;
/*
The sender propagates a job's volume update to the receiver.
Data type: [jobId, volume]
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
The sender provides the global rank of the client node which initiated
a certain job.
Data type: [jobId, clientRank]
*/
const int MSG_SEND_CLIENT_RANK = 11;
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
/* For incremental jobs. Unsupported as of now */
const int MSG_NOTIFY_JOB_REVISION = 17;
/* For incremental jobs. Unsupported as of now */
const int MSG_QUERY_JOB_REVISION_DETAILS = 18;
/* For incremental jobs. Unsupported as of now */
const int MSG_SEND_JOB_REVISION_DETAILS = 19;
/* For incremental jobs. Unsupported as of now */
const int MSG_CONFIRM_JOB_REVISION_DETAILS = 20;
/* For incremental jobs. Unsupported as of now */
const int MSG_SEND_JOB_REVISION_DATA = 21;
/* For incremental jobs. Unsupported as of now */
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
A client tells another client that the sender is now out of jobs to introduce to the system.
Used to detect early termination.
*/
const int MSG_CLIENT_FINISHED = 26;
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
The sender notifies the receiver that the job result the receiver just sent
is obsolete and will not be needed. It does not need to be preserved.
*/
const int MSG_NOTIFY_RESULT_OBSOLETE = 31;

/*
Pseudo-tag representing all tags that can be received at any time.
NOT a tag to be used outside of MyMpi.*.
*/
const int MSG_ANYTIME = 1337;

#endif
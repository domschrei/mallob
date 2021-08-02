
#ifndef DOMPASCH_MALLOB_CLIENT
#define DOMPASCH_MALLOB_CLIENT

#include <string>
#include <set>
#include <atomic>

#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "data/job_description.hpp"
#include "util/sys/threading.hpp"
#include "data/job_file_adapter.hpp"
#include "data/job_metadata.hpp"
#include "comm/sysstate.hpp"
#include "util/sys/background_worker.hpp"

#define SYSSTATE_ENTERED_JOBS 0
#define SYSSTATE_PARSED_JOBS 1
#define SYSSTATE_SCHEDULED_JOBS 2
#define SYSSTATE_PROCESSED_JOBS 3

struct JobByArrivalComparator {
    inline bool operator() (const JobMetadata& struct1, const JobMetadata& struct2) const {
        if (struct1.description->getArrival() != struct2.description->getArrival())
            return (struct1.description->getArrival() < struct2.description->getArrival());
        return struct1.description->getId() < struct2.description->getId();
    }
};

class Client {

private:
    MPI_Comm _comm;
    int _world_rank;
    Parameters& _params;

    // For incoming job meta data. Full instance is NOT read yet.
    // Filled from JobFileAdapter, emptied by instance reader thread,
    // ready jobs are put in the ready queue.
    std::set<JobMetadata, JobByArrivalComparator> _incoming_job_queue;
    std::atomic_int _num_incoming_jobs = 0;
    // Safeguards _incoming_job_queue.
    Mutex _incoming_job_lock;

    // For jobs which have been fully read and initialized
    // and whose prerequisites for activation are met.
    std::list<std::shared_ptr<JobDescription>> _ready_job_queue;
    std::atomic_int _num_ready_jobs = 0;
    // Safeguards _ready_job_queue.
    Mutex _ready_job_lock;

    // For active jobs in the system. ONLY ACCESSIBLE FROM CLIENT'S MAIN THREAD.
    std::map<int, std::shared_ptr<JobDescription>> _active_jobs;
    
    // Collection of job IDs which finished (for checking dependencies) and their corresponding revision.
    struct DoneInfo {int revision; Checksum lastChecksum;};
    robin_hood::unordered_flat_set<int, robin_hood::hash<int>> _recently_done_jobs;
    robin_hood::unordered_flat_map<int, DoneInfo, robin_hood::hash<int>> _done_jobs;
    // Safeguards _done_jobs.
    Mutex _done_job_lock; 

    std::map<int, int> _root_nodes;
    std::set<int> _client_ranks;
    SysState<4> _sys_state;

    BackgroundWorker _instance_reader;
    std::unique_ptr<JobFileAdapter> _file_adapter;

    // Maps a job ID to the ID of the message handle transferring its description
    std::map<int, std::pair<int, int>> _transfer_msg_id_to_job_id_rev;
    // Number of jobs with a loaded description (taking memory!)
    std::atomic_int _num_loaded_jobs = 0;
    Mutex _finished_msg_ids_mutex;
    std::vector<int> _finished_msg_ids;

public:
    Client(MPI_Comm comm, Parameters& params, std::set<int> clientRanks)
        : _comm(comm), _world_rank(MyMpi::rank(MPI_COMM_WORLD)), 
        _params(params), _client_ranks(clientRanks), _sys_state(_comm) {}
    ~Client();
    void init();
    void mainProgram();

    // Callback from JobFileAdapter when a new job's meta data were read
    void handleNewJob(JobMetadata&& data);

private:
    void readIncomingJobs(Logger log);
    
    void handleOfferAdoption(MessageHandle& handle);
    void handleJobDone(MessageHandle& handle);
    void handleAbort(MessageHandle& handle);
    void handleSendJobResult(MessageHandle& handle);
    void handleClientFinished(MessageHandle& handle);
    void handleExit(MessageHandle& handle);
    void handleJobDescriptionSent(int msgId);

    int getMaxNumParallelJobs();
    void introduceNextJob();
    void finishJob(int jobId, bool hasIncrementalSuccessors);
    
};

#endif

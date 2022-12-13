
#ifndef DOMPASCH_MALLOB_CLIENT
#define DOMPASCH_MALLOB_CLIENT

#include <string>
#include <set>
#include <atomic>

#include "comm/mympi.hpp"
#include "util/params.hpp"
#include "data/job_description.hpp"
#include "util/sys/threading.hpp"
#include "interface/json_interface.hpp"
#include "data/job_metadata.hpp"
#include "comm/sysstate.hpp"
#include "util/sys/background_worker.hpp"
#include "util/periodic_event.hpp"
#include "interface/api/api_connector.hpp"
#include "comm/msg_queue/message_subscription.hpp"

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

/*
Primary actor in the system who is responsible for introducing jobs from an external interface
and reporting results back over this interface. There is at most one Client instance for each PE.
*/
class Client {

private:
    MPI_Comm _comm;
    int _world_rank;
    Parameters& _params;

    std::list<MessageSubscription> _subscriptions;

    // For incoming job meta data. Full instance is NOT read yet.
    // Filled from JobFileAdapter, emptied by instance reader thread,
    // ready jobs are put in the ready queue.
    std::set<JobMetadata, JobByArrivalComparator> _incoming_job_queue;
    std::atomic_int _num_incoming_jobs = 0;
    // Safeguards _incoming_job_queue.
    Mutex _incoming_job_lock;
    ConditionVariable _incoming_job_cond_var;
    
    std::atomic_long _next_arrival_time_millis = -1;
    std::set<float> _arrival_times;
    Mutex _arrival_times_lock;

    // For jobs which have been fully read and initialized
    // and whose prerequisites for activation are met.
    std::list<std::unique_ptr<JobDescription>> _ready_job_queue;
    std::atomic_int _num_ready_jobs = 0;
    // Safeguards _ready_job_queue.
    Mutex _ready_job_lock;

    // For jobs which could not be initialized.
    std::list<std::string> _failed_job_queue;
    std::atomic_int _num_failed_jobs = 0;
    // Safeguards _failed_job_queue.
    Mutex _failed_job_lock;

    // For active jobs in the system. ONLY ACCESSIBLE FROM CLIENT'S MAIN THREAD.
    std::map<int, std::unique_ptr<JobDescription>> _active_jobs;
    
    // Collection of job IDs which finished (for checking dependencies) and their corresponding revision.
    struct DoneInfo {int revision; Checksum lastChecksum;};
    robin_hood::unordered_flat_set<int, robin_hood::hash<int>> _recently_done_jobs;
    robin_hood::unordered_flat_map<int, DoneInfo, robin_hood::hash<int>> _done_jobs;
    // Safeguards _done_jobs.
    Mutex _done_job_lock; 

    std::list<std::future<void>> _done_job_futures;

    std::atomic_int _num_jobs_to_interrupt = 0;
    robin_hood::unordered_set<int> _jobs_to_interrupt;
    Mutex _jobs_to_interrupt_lock;

    std::map<int, int> _root_nodes;
    std::set<int> _client_ranks;
    SysState<4> _sys_state;

    std::unique_ptr<JsonInterface> _json_interface;
    std::vector<Connector*> _interface_connectors;
    APIConnector* _api_connector;
    BackgroundWorker _instance_reader;

    // Number of jobs with a loaded description (taking memory!)
    std::atomic_int _num_loaded_jobs = 0;
    Mutex _finished_msg_ids_mutex;
    std::vector<int> _finished_msg_ids;

    PeriodicEvent<100> _periodic_check_done_jobs;

public:
    Client(MPI_Comm comm, Parameters& params)
        : _comm(comm), _world_rank(MyMpi::rank(MPI_COMM_WORLD)), 
        _params(params), _sys_state(_comm, params.sysstatePeriod(), SysState<4>::ALLREDUCE) {}
    ~Client();
    void init();
    void advance();

    // Callback from JobFileAdapter when a new job's meta data were read
    void handleNewJob(JobMetadata&& data);

    int getInternalRank();
    std::string getFilesystemInterfacePath();
    std::string getSocketPath();
    APIConnector& getAPI();

private:
    void readIncomingJobs();
    
    void handleOfferAdoption(MessageHandle& handle);
    void handleJobDone(MessageHandle& handle);
    void handleAbort(MessageHandle& handle);
    void handleSendJobResult(MessageHandle& handle);
    void handleClientFinished(MessageHandle& handle);
    void handleExit(MessageHandle& handle);

    int getMaxNumParallelJobs();
    void introduceNextJob();
    void finishJob(int jobId, bool hasIncrementalSuccessors);
    
};

#endif

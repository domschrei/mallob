
#pragma once

#include <list>

#include "app/job.hpp"
#include "util/sys/threading.hpp"
#include "util/sys/background_worker.hpp"
#include "util/logger.hpp"

class JobGarbageCollector {

private:
    BackgroundWorker _worker;
    std::list<Job*> _job_destruct_queue;
    std::list<Job*> _jobs_to_free;
    Mutex _mtx;
    ConditionVariable _cond_var;
    std::atomic_int _num_stored_jobs = 0;

public:
    JobGarbageCollector() {
        _worker.run([&]() {
            Proc::nameThisThread("JobJanitor");
            run();
        });
    }

    void orderDeletion(Job* jobPtr) {
        _job_destruct_queue.push_back(jobPtr);
    }

    std::atomic_int& numStoredJobs() {
        return _num_stored_jobs;
    }

    void forgetOldJobs() {

        // Find "forgotten" jobs in destruction queue which can now be destructed
        for (auto it = _job_destruct_queue.begin(); it != _job_destruct_queue.end(); ) {
            Job* job = *it;
            if (job->isDestructible()) {
                LOG(V4_VVER, "%s ready for destruction\n", job->toStr());
                // Move pointer to "free" queue emptied by janitor thread
                {
                    auto lock = _mtx.getLock();
                    _jobs_to_free.push_back(job);
                }
                _cond_var.notify();
                it = _job_destruct_queue.erase(it);
            } else ++it;
        }
    }

    bool hasJobsLeftInDestructQueue() const {
        return !_job_destruct_queue.empty();
    }

    ~JobGarbageCollector() {
        _worker.stopWithoutWaiting();
        _cond_var.notify();
        _worker.stop();
    }

private:
    void run() {

        auto lg = Logger::getMainInstance().copy("<Janitor>", ".janitor");
        LOGGER(lg, V3_VERB, "tid=%lu\n", Proc::getTid());
        
        while (_worker.continueRunning() || _num_stored_jobs > 0) {
        
            std::list<Job*> copy;
            {
                // Try to fetch the current jobs to free
                auto lock = _mtx.getLock();
                _cond_var.waitWithLockedMutex(lock, [&]() {
                    return !_worker.continueRunning() || !_jobs_to_free.empty();
                });
                if (!_worker.continueRunning() && _jobs_to_free.empty() && _num_stored_jobs == 0)
                    break;
                
                // Copy jobs to free to local list
                for (Job* job : _jobs_to_free) copy.push_back(job);
                _jobs_to_free.clear();
            }

            LOGGER(lg, V5_DEBG, "Found %i job(s) to delete\n", copy.size());
            
            // Free each job
            for (Job* job : copy) {
                int id = job->getId();
                LOGGER(lg, V4_VVER, "DELETE #%i\n", id);
                delete job;
                Logger::getMainInstance().mergeJobLogs(id);
                _num_stored_jobs--;
            }

            if (!_worker.continueRunning()) usleep(100 * 1000); // wait for last jobs to finish
        }
    }
};

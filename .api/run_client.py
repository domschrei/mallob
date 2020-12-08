
import random
import numpy
import time
from scipy.stats import truncnorm
import os.path



def get_truncated_normal(mean=0, sd=1, low=0, upp=10):
    return int(truncnorm((low - mean) / sd, (upp - mean) / sd, loc=mean, scale=sd).rvs().round())

class Job:
    
    def __init__(self, user, name, filename, priority=1, wc_timeout=0, cpu_timeout=0):
        self._user = user
        self._name = name
        self._filename = filename
        self._priority = priority
        self._wc_timeout = wc_timeout
        self._cpu_timeout = cpu_timeout

    def to_json(self):
        return '{ "user": "%s", "name": "%s", "file": "%s", "priority": %.3f, "wallclock-limit": "%s", "cpu-limit": "%s" }' % (self._user, self._name, self._filename, self._priority, self._wc_timeout, self._cpu_timeout)
    
    def get_json_filename(self):
        return self._user + "." + self._name + ".json"

class Client:
    
    def __init__(self, name, arrival, priority=1):
        self._name = name
        self._arrival = arrival
        self._priority = priority
        self._stream = []
        self._running_job_id = 1
    
    def add_job_to_stream(self, filename, priority=1, wc_timeout=0, cpu_timeout=0):
        self._stream += [Job(self._name, "job-" + str(self._running_job_id), filename, priority=self._priority*priority, wc_timeout=wc_timeout, cpu_timeout=cpu_timeout)]
        self._running_job_id += 1
    
    def has_next_job(self):
        return bool(self._stream)
    
    def get_next_job(self):
        next_job = self._stream[0]
        self._stream = self._stream[1:]
        return next_job
    
    def user_to_json(self):
        return '{ "id": "%s", "priority": %.3f }' % (self._name, self._priority)
        


def get_instance_filename(inst_id):
    # TODO implement properly
    if inst_id % 2 == 0:
        return "/home/dominik/workspace/sat_instances/test_sat.cnf"
    else:
        return "/home/dominik/workspace/sat_instances/test_unsat.cnf"



global_client_id = 1
global_job_id = 1

min_stream_length = 1
max_stream_length = 100
mean_stream_length = 1
stdv_stream_length = 3

num_clients = 100
client_interarrival_time = 1
wc_limit_per_job = 600
cpu_limit_per_job = 100000

global_time = 0
def create_random_client():
    global global_job_id, global_client_id, global_time
    
    c = Client("c-" + str(global_client_id), global_time, priority=1.0)
    global_client_id += 1
    
    streamlen = get_truncated_normal(mean_stream_length, stdv_stream_length, min_stream_length, max_stream_length)
    for _ in range(streamlen):
        c.add_job_to_stream(get_instance_filename(global_job_id))
        global_job_id += 1
    
    global_time += + numpy.random.exponential(client_interarrival_time)
    return c

def get_max_duration_of_client(client):
    (_,streams) = client
    return max([len(s) for s in streams]) * wc_timeout_per_job

def introduce_job(job):
    print("  %s introducing job %s" % (job._user, job._name))
    with open("jobs.0/new/" + job.get_json_filename(), "w") as f:
        f.write(job.to_json())

def elapsed_time(t_start):
    return 0.001 * 0.001 * 0.001 * (time.time_ns() - t_start)


clients = []
for i in range(num_clients):
    c = create_random_client()
    with open("users/" + c._name + ".json", "w") as f:
        f.write(c.user_to_json())
    clients += [c]

clients.sort(key = lambda c: c._arrival)
active_jobs = [None for c in clients]
t_start = time.time_ns()

any_left = True
while any_left:
    elapsed = elapsed_time(t_start)
    any_left = False
    
    for i in range(len(clients)):
        c = clients[i]
        job = active_jobs[i]
        
        if c._arrival > elapsed or active_jobs[i] is not None:
            any_left = True
        
        if active_jobs[i] is None and c._arrival <= elapsed and c.has_next_job():
            # Introduce first job
            print("%.2f Arrival of %s" % (elapsed, c._name,))
            active_jobs[i] = c.get_next_job()
            introduce_job(active_jobs[i])
        
        elif active_jobs[i] is not None:
            # Check if job finished
            last_file = "jobs/done/" + active_jobs[i].get_json_filename()
            if os.path.isfile(last_file):
                # -- job finished
                print("%.2f %s finished" % (elapsed, c._name + "." + active_jobs[i]._name))
                os.remove(last_file)
                # Introduce next job
                if c.has_next_job():
                    active_jobs[i] = c.get_next_job()
                    introduce_job(active_jobs[i])
                else:
                    print("%.2f %s completed" % (elapsed, c._name))
                    active_jobs[i] = None
    
    time.sleep(0.1)

print("%.2f done." % (elapsed_time(t_start),))
 

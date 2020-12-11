
import random
import numpy
import time
from scipy.stats import truncnorm
import os.path
from os import listdir
from os.path import isfile, join
import json
import copy

"""
Given an integer inst_id, returns a SAT instance associated to that ID.
The associated instances may cycle, e.g., map (inst_id % num_instances) to an instance.
"""
def get_instance_filename(inst_id):

    # Read benchmark file
    global global_benchmark
    if not global_benchmark:
        global_benchmark = []
        for line in open(".api/benchmark_sat2020", "r").readlines():
            str = line.rstrip()
            if str:
                global_benchmark += [str]
        global_benchmark.sort()

    # Select a file randomly
    files = copy.deepcopy(global_benchmark)
    r = random.Random(int(inst_id / len(global_benchmark)))
    r.shuffle(files)
    return "instances/" + files[inst_id % len(files)]

"""
Represents a single job of a particular client.
"""
class Job:
    
    def __init__(self, user, name, filename, priority=1, wc_timeout=0, cpu_timeout=0):
        self._user = user
        self._name = name
        self._filename = filename
        self._priority = priority
        self._wc_timeout = wc_timeout
        self._cpu_timeout = cpu_timeout

    def to_json(self):
        return '{ "user": "%s", "name": "%s", "file": "%s", "priority": %.3f, "wallclock-limit": "%s", "cpu-limit": "%s" }' % (
            self._user, self._name, self._filename, self._priority, self._wc_timeout, self._cpu_timeout)
    
    def get_json_filename(self):
        return self._user + "." + self._name + ".json"

"""
Represents a particular client with a stream (sequence) of subsequent jobs to be processed.
"""
class Client:
    
    def __init__(self, name, arrival, priority=1):
        self._name = name
        self._arrival = arrival
        self._priority = priority
        self._stream = []
        self._running_job_id = 1
    
    def add_job_to_stream(self, filename, priority=1, wc_timeout=0, cpu_timeout=0):
        self._stream += [Job(self._name, "job-" + str(self._running_job_id), filename, 
                        priority=self._priority*priority, wc_timeout=wc_timeout, cpu_timeout=cpu_timeout)]
        self._running_job_id += 1
    
    def has_next_job(self):
        return bool(self._stream)
    
    def get_next_job(self):
        next_job = self._stream[0]
        self._stream = self._stream[1:]
        return next_job
    
    def user_to_json(self):
        return '{ "id": "%s", "priority": %.3f }' % (self._name, self._priority)
    
    def get_max_stream_duration(self):
        return sum([j._wc_timeout if j._wc_timeout > 0 else float('inf') for j in self._stream])

# Running IDs for clients and jobs
global_client_id = 1
global_job_id = 0

# Global start time
global_starttime = time.time_ns()

# List of instance file names from which new jobs are drawn
global_benchmark = None

"""
Returns the elapsed time since program start in seconds (float)
"""
def elapsed_time():
    return 0.001 * 0.001 * 0.001 * (time.time_ns() - global_starttime)

"""
Logs a message msg formatted (%-operator) with a tuple args.
"""
def log(msg, args=()):
    print("[%.2f]" % (elapsed_time(),), msg % args)

"""
Truncates a normal distribution at lower and upper limits.
"""
def get_truncated_normal(mean=0, sd=1, low=0, upp=10):
    return int(truncnorm((low - mean) / sd, (upp - mean) / sd, loc=mean, scale=sd).rvs().round())

"""
Creates a new client with a random stream of jobs.
"""
def create_random_client(arrival_time):
    global global_job_id, global_client_id
    
    c = Client("c-" + str(global_client_id), arrival_time, priority=1.0)
    global_client_id += 1
    
    streamlen = get_truncated_normal(mean_stream_length, stdv_stream_length, min_stream_length, max_stream_length)
    for _ in range(streamlen):
        c.add_job_to_stream(get_instance_filename(global_job_id))
        global_job_id += 1
    
    return c

"""
Takes a job instance and writes a JSON file into the mallob API directory.
"""
def introduce_job(job):
    log("%s introduces job %s (%s)", (job._user, job._name, job._filename))
    with open(".api/jobs.0/new/" + job.get_json_filename(), "w") as f:
        f.write(job.to_json())


# Statistical parameters for the generation of job streams
min_stream_length = 1
max_stream_length = 100
mean_stream_length = 1
stdv_stream_length = 3

# Number and arrival frequency of clients
num_clients = 64
client_interarrival_time = 2.00

# Resource limits given to each job (overriding mallob's global options!)
wc_limit_per_job = 600
cpu_limit_per_job = 100000

# Create a number of random clients with a random job stream each
clients = []
arrival_time = 0
for i in range(num_clients):
    c = create_random_client(arrival_time)
    with open(".api/users/" + c._name + ".json", "w") as f:
        f.write(c.user_to_json())
    clients += [c]
    arrival_time += + numpy.random.exponential(client_interarrival_time)

# Remember the currently active job for each client
active_jobs = [None for c in clients]

# Main loop where jobs are introduced and finished job information is removed
any_left = True
while any_left:
    time.sleep(0.1)
    elapsed = elapsed_time()
    any_left = False
    
    # For each client
    for i in range(len(clients)):
        c = clients[i]
        job = active_jobs[i]
        
        # Is client not yet active (not arrived yet) or does it have an active job?
        if c._arrival > elapsed or active_jobs[i] is not None:
            # -> Program will resume for another loop
            any_left = True
        
        # Did client arrive just now?
        if active_jobs[i] is None and c._arrival <= elapsed and c.has_next_job():
            # -> Introduce first job
            log("Client %s arrives", (c._name,))
            active_jobs[i] = c.get_next_job()
            introduce_job(active_jobs[i])
            any_left = True
        
        elif active_jobs[i] is not None:
            # Check if job finished
            done_file = ".api/jobs.0/done/" + active_jobs[i].get_json_filename()
            if os.path.isfile(done_file):
                # -- job finished

                f = open(done_file, 'r')
                try:
                    j = json.load(f)
                    log("%s finished (response time: %.3f, result code: %i)", (c._name + "." + active_jobs[i]._name, j["result"]["responsetime"], j["result"]["resultcode"]))
                    f.close()
                    os.remove(done_file)
                    
                    # Introduce next job
                    if c.has_next_job():
                        active_jobs[i] = c.get_next_job()
                        introduce_job(active_jobs[i])
                    else:
                        log("%s completed", (c._name,))
                        active_jobs[i] = None

                except json.decoder.JSONDecodeError as e:
                    # Could not parse JSON file - probably it is still being written. Try again later
                    f.close()

log("Done.")

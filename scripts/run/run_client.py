#!/usr/bin/env python3

import random
import numpy
import numpy.random
from numpy.random import RandomState
import time
from scipy.stats import truncnorm
from scipy.stats import lognorm
import os.path
from os import listdir
from os.path import isfile, join
import json
import copy
from optparse import OptionParser
import psutil

"""
Given an integer inst_id, returns a SAT instance associated to that ID.
The associated instances may cycle, e.g., map (inst_id % num_instances) to an instance.
"""
def get_instance_filename(options, inst_id):

    # Read benchmark file
    global global_benchmark
    if not global_benchmark:
        global_benchmark = []
        for line in open(options.benchmark_file, "r").readlines():
            str = line.rstrip()
            if str:
                global_benchmark += [str]
        global_benchmark.sort()

    # Select a file randomly
    files = copy.deepcopy(global_benchmark)
    r = random.Random(options.seed + int(inst_id / len(global_benchmark)))
    r.shuffle(files)
    return "instances/" + files[inst_id % len(files)]

"""
Represents a single job of a particular client.
"""
class Job:
    
    def __init__(self, user, name, filename, priority=1, wc_timeout=0, cpu_timeout=0, max_demand=0, arrival=0, dependencies=[]):
        self._user = user
        self._name = name
        self._filename = filename
        self._priority = priority
        self._wc_timeout = wc_timeout
        self._cpu_timeout = cpu_timeout
        self._max_demand = max_demand
        self._arrival = arrival
        self._dependencies = dependencies

    def to_json(self):
        d_str = str(self._dependencies).replace("'", "\"")
        return '{ "user": "%s", "name": "%s", "file": "%s", "priority": %.3f, "wallclock-limit": "%s", "cpu-limit": "%s", "max-demand": %i, "arrival": %.3f, "dependencies": %s }' % (
            self._user, self._name, self._filename, self._priority, self._wc_timeout, self._cpu_timeout, self._max_demand, self._arrival, d_str)
    
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
    
    def add_job_to_stream(self, filename, priority=1, wc_timeout=0, cpu_timeout=0, max_demand=0, arrival=0):
        dependencies = [self._stream[-1]._user+"."+self._stream[-1]._name] if self._stream else []
        self._stream += [Job(self._name, "job-" + str(self._running_job_id), filename,
                        priority=self._priority*priority, wc_timeout=wc_timeout, cpu_timeout=cpu_timeout, 
                        max_demand=max_demand, arrival=max(arrival,self._arrival), dependencies=dependencies)]
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
Returns a single random sample from a normal distribution truncated at lower and upper limits.
"""
def get_truncated_normal(mean=0, sd=1, low=0, upp=10):
    global random_state
    if low == upp:
        return low
    return int(truncnorm((low - mean) / sd, (upp - mean) / sd, loc=mean, scale=sd).rvs(random_state=random_state).round())

"""
Returns a single random sample from a log-normal distribution truncated at lower and upper limits.
"""
def get_lognorm(s, low=1, upp=100):
    global random_state
    if low == upp:
        return low
    val = lognorm(s).rvs(random_state=random_state)
    return max(low, min(upp, int(val.round())))

"""
Creates a new client with a random stream of jobs.
"""
def create_random_clients(options, arrival_time):
    global global_job_id, global_client_id
    
    stream_width = get_lognorm(options.shape_stream_width, low=options.min_stream_width, upp=options.max_stream_width)
    cs = []

    for i in range(stream_width):
        c = Client("c" + str(global_client_id) + "_" + str(i), arrival_time, priority=1.0)
        
        streamlen = get_truncated_normal(options.mean_stream_length, options.stddev_stream_length, options.min_stream_length, options.max_stream_length)
        for _ in range(streamlen):
            c.add_job_to_stream(get_instance_filename(options, global_job_id))
            global_job_id += 1
        cs += [c]

    global_client_id += 1
    return cs

"""
Takes a job instance and writes a JSON file into the mallob API directory.
"""
def introduce_job(job, options):
    p = int(random.random() * options.num_client_processes)
    log("%s introduces job %s (%s) to .api/jobs.%i/", (job._user, job._name, job._filename, p))
    with open(".api/jobs." + str(p) + "/new/" + job.get_json_filename(), "w") as f:
        f.write(job.to_json())


def main():
    global global_starttime
    global random_state

    usage = "usage: %prog [options] <benchmark_file>"
    parser = OptionParser(usage=usage)
    
    parser.add_option("-n", "--num-clients",          type="int",   dest="num_clients",          default="1",     help="number of clients arriving")
    parser.add_option("-i", "--interarrival-time",    type="float", dest="interarrival_time",    default="10",    help="interarrival time of clients")
    parser.add_option("-p", "--num-client-processes", type="int",   dest="num_client_processes", default="1",     help="number of client processes operating mallob's API")
    
    parser.add_option("-m", "--min-stream-length",    type="int",   dest="min_stream_length",    default="1",     help="minimum length of any job stream")
    parser.add_option("-x", "--max-stream-length",    type="int",   dest="max_stream_length",    default="100",   help="maximum length of any job stream")
    parser.add_option("-e", "--mean-stream-length",   type="float", dest="mean_stream_length",   default="1",     help="mean length of job streams")
    parser.add_option("-d", "--stddev-stream-length", type="float", dest="stddev_stream_length", default="3",     help="standard deviation of length of job streams")
    
    parser.add_option("-M", "--min-stream-width",     type="int",   dest="min_stream_width",     default="1",     help="minimum width of any job stream")
    parser.add_option("-X", "--max-stream-width",     type="int",   dest="max_stream_width",     default="1",     help="maximum width of any job stream")
    parser.add_option("-S", "--shape-stream-width",   type="float", dest="shape_stream_width",   default="1",     help="shape parameter for log-normal width of job streams")
    
    parser.add_option("-s", "--seed",                 type="float", dest="seed",                 default="0",     help="random seed (0 for no seed)")
    parser.add_option("-w", "--wallclock-limit",      type="float", dest="wc_limit_per_job",     default="0",     help="wallclock limit per job in seconds (0 for no limit)")
    parser.add_option("-c", "--cpu-limit",            type="float", dest="cpu_limit_per_job",    default="0",     help="cpu limit per job in CPU seconds (0 for no limit)")

    parser.add_option("-I", "--instant-output", action="store_true", dest="instant_output",      default="False", help="Instantly output all job files with their arrival time")
    
    (options, args) = parser.parse_args()
    if len(args) != 1:
        print("Please provide a benchmark file to read jobs from.")
        exit(1)

    options.benchmark_file = args[0]
    
    if options.seed != 0:
        random.seed(options.seed)
        random_state = RandomState(int(1000*options.seed))
    else:
        random_state = RandomState()

    # Create a number of random clients with a random job stream each
    log("Creating clients ...")
    clients = []
    arrival_time = 0
    for i in range(options.num_clients):
        cs = create_random_clients(options, arrival_time)
        for c in cs:
            with open(".api/users/" + c._name + ".json", "w") as f:
                f.write(c.user_to_json())
            clients += [c]
        arrival_time += + numpy.random.exponential(options.interarrival_time)

    # Remember the currently active job for each client
    active_jobs = [None for c in clients]
    log("Clients set up.")

    if not options.instant_output:
        # Wait for mallob process
        log("Waiting for mallob ...")
        mallob_active = False
        while not mallob_active:
            for process in psutil.process_iter():
                if process.name() == "mallob":
                    mallob_active = True
                    break
            time.sleep(0.1)
        log("Process named \"mallob\" is active!")

    global_starttime = time.time_ns()
    log("Launching clients.")

    # Main loop where jobs are introduced and finished job information is removed
    any_left = True
    while any_left:
        time.sleep(0.1)
        elapsed = float("inf") if options.instant_output else elapsed_time()
        any_left = False
        
        # For each client
        for i in range(len(clients)):
            c = clients[i]
            
            # Is client not yet active (not arrived yet) or does it have an active job?
            if c._arrival > elapsed or active_jobs[i] is not None:
                # -> Program will resume for another loop
                any_left = True
            
            # Did client arrive just now?
            if active_jobs[i] is None and c._arrival <= elapsed and c.has_next_job():
                # -> Introduce first job
                log("Client %s arrives", (c._name,))
                active_jobs[i] = c.get_next_job()
                introduce_job(active_jobs[i], options)
                any_left = True
            
            elif active_jobs[i] is not None:
                if options.instant_output:
                    # Introduce next job directly
                    # Introduce next job
                    if c.has_next_job():
                        active_jobs[i] = c.get_next_job()
                        introduce_job(active_jobs[i], options)
                    else:
                        log("%s completed", (c._name,))
                        active_jobs[i] = None
                else:
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
                                introduce_job(active_jobs[i], options)
                            else:
                                log("%s completed", (c._name,))
                                active_jobs[i] = None

                        except json.decoder.JSONDecodeError:
                            # Could not parse JSON file - probably it is still being written. Try again later
                            f.close()

    log("Done.")

if __name__ == "__main__":
    main()

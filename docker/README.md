
# Mallob Docker Setup

Read these instructions to learn how to build and execute Mallob in Docker containers.

## Building

From this directory, build the Mallob image as follows:
```bash
docker build -t mallob:leader . -f Dockerfile
```
Edit `Dockerfile` in order to adjust the repository URL, the specific commit to be used, and/or the build arguments.

## Execution

Enter a container of this image as follows:
```bash
docker run --shm-size=32g -w /mallob -it mallob:leader /bin/bash
```

In the container, you can execute Mallob as in the following examples:
```bash
# Solve a single SAT instance with processes as specified in "hostfile.txt" (configure yourself!).
# The value of "slots" specifies the number of processes to be spawned (>= 1) at the particular host.
echo "localhost slots=4" > hostfile.txt
MALLOB_HOSTFILE=hostfile.txt bash docker/run-in-docker-mono.sh instances/r3unsat_200.cnf -t=2 -v=4

# Solve a single SAT instance with four processes on this machine only.
MALLOB_NP=4 bash docker/run-in-docker-mono.sh instances/r3unsat_200.cnf -t=2 -v=4

# Execute Mallob as a generic platform, taking jobs via some interface.
MALLOB_NP=4 bash docker/run-in-docker.sh -v=4 [options ...]
```

`run-in-docker-mono.sh` presets some sane default options, e.g., setting the number of solvers and normalizing the sharing volume based on the local machine's number of threads. See for yourself whether these defaults are suitable for you.  
`run-in-docker.sh` leaves all configuration to the caller.  
In any case, all of Mallob's program options can be overriden by appending options to the script call (as for -t and -v in the above examples).

As of yet, we do not provide fine-grained documentation for running Mallob within multiple (distributed) Docker containers. On a high level, the following steps are required:

* Set up a Docker network which connects the different nodes.
* With all containers running, assemble a hostfile, which features all nodes' addresses, at a particular "leader" node.  
* In the leader node container, execute the script `run-in-docker-mono.sh` with the assembled hostfile.

The documentation for the SAT competition 2023 essentially follows the same setup (with different scripts) and may be a useful resource for your particular setup:
https://github.com/domschrei/aws-batch-comp-infrastructure-sample/blob/mallob23/docker/README-Solver-Development.md#running-mallob

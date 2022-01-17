#!/bin/bash

set -e

# Parameters to supply:
# 1st parameter: number of MPI processes to start
# 2nd parameter (optional): a text file containing one CNF problem file path per line

# 2nd parameter missing?
if [ -z $2 ]; then
    # -- collect all CNF problems in the instances/ directory, write a text file
    cd instances
    > _benchmark_local
    for f in *.cnf ; do
       echo $f >> _benchmark_local
    done
    shuf _benchmark_local -o _benchmark_local # shuffle lines randomly
    cd ..
    benchmarkfile="instances/_benchmark_local"
else
    # use the user-supplied benchmark file
    benchmarkfile=$2
fi

# Import important functions (see scripts/systest_commons.sh)
testcount=1
source $(dirname "$0")/systest_commons.sh

# Directory setup
mkdir -p .api/jobs.0/
mkdir -p .api/jobs.0/{introduced,in,out}/
cleanup

function create_independent_jobs() {
    i=1
    while read -r instance; do
        # Parameters: job name, instance path, wallclock limit, arrival, dependencies, application (SAT or DUMMY)
        wclimit=300s introduce_job solve-$i instances/$instance
        echo "solve-$i : $instance"
        i=$((i+1))
    done < $benchmarkfile
    export numjobs=$i
}

function sample_exponential() {
    lambda="$1"
    echo "- $lambda * l( 1 - 0.0001 * $((1 + $RANDOM % 9999)) )"|bc -l
}

function create_job_chains() {
    
    ninstances=$(cat $benchmarkfile|wc -l)
    interarrivaltime=1
    jobsperclient_lambda=1.5
    timeperjob=10
    maxtime=300
    
    instno=1
    i=1
    t=1
    c=1
    
    while true; do
        if (( $(echo "$t > $maxtime" |bc -l) )); then break; fi
        
        #jobsthisclient=$(sample_exponential $jobsperclient_lambda)
        jobsthisclient=1

        added=false
        for k in $(seq 1 $jobsthisclient); do
            added=true
            dep=""
            if [ $k -gt 1 ]; then dep="\"admin.solve-$c-$(($k-1))\"" ; fi
            instance=instances/$(sed "${instno}q;d" $benchmarkfile)
            echo "solve-$c-$k : arrival@$t deps=$dep $instance"
            application=SAT wclimit=${timeperjob}s arrival=$t dependency="$dep" introduce_job solve-$c-$k "$instance"
            i=$(($i+1))
            instno=$(($instno+1))
            if [ $instno -gt $ninstances ]; then 
                instno=1
            fi
        done

        if $added; then
            t=$( echo $t + $(sample_exponential $interarrivaltime) |bc -l )
            c=$((c+1))
        fi
    done

    export numjobs=$i
}


# Generate chain of independent jobs
#create_independent_jobs

# Generate clients, each with a chain of dependent jobs
create_job_chains

echo "Generated $numjobs jobs"

if [ -z $1 ] || [ $1 == 0 ]; then
    echo "Not running (#workers missing)"
    exit 0
fi

# Set options
options="-t=4 -c=1 -ajpc=$(($1-2)) -satsolver=kkccllg -v=5 -J=$numjobs -warmup -y=0.1"

# Launch Mallob with a unique run ID for a logging directory
runid="sched_$(hostname)_$(git rev-parse --short HEAD)_np${1}_"$(echo $options|sed 's/-//g'|sed 's/=//g'|sed 's/ /_/g')
RDMAV_FORK_SAFE=1 PATH=build/:$PATH nohup mpirun -np $1 --oversubscribe build/mallob -log=runs/$runid $options 2>&1 > OUT &

echo "Use \"tail -f OUT\" to follow output"
echo "Use \"killall mpirun\" to terminate"
sleep 1; echo "" # To mend ugly linebreak done by nohup

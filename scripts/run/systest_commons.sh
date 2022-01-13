#!/bin/bash

function cleanup() {
    set +e
    rm .api/jobs.0/*/*.json _systest _incremental_jobs-* /dev/shm/edu.kit.iti.mallob* 2> /dev/null
    set -e
}

function error() {
    echo "ERROR: $@"
    exit 1
}

function run_incremental() {
    echo "$BASHPID: Reading incremental job from $1"
    while read line; do
                
        file=$(echo $line|awk '{print $1}')
        result=$(echo $line|awk '{print $2}')
        
        echo "$BASHPID: Initiate $file"
        cp .api/jobs.0/{introduced,in}/$file

        if [ "$result" == "" ] ; then
            continue
        fi

        donefile=.api/jobs.0/out/admin.$file
        while [ ! -f $donefile ]; do sleep 0.1 ; done
        
        echo "$BASHPID: Found $donefile"
        
        if ! grep -q '"resultstring": "UNKNOWN"' $donefile ; then
            if [ $result == unsat ] ; then
                if ! grep -q '"resultstring": "UNSAT"' $donefile ; then
                    error "Expected result UNSAT for $file was not found."
                fi
            elif [ $result == sat ] ; then
                if ! grep -q '"resultstring": "SAT"' $donefile ; then
                    error "Expected result SAT for $file was not found."
                fi
            fi
        fi
        rm $donefile
    done < $1
    echo "$BASHPID: Done." 
}

function run() {
    echo "[$testcount] -np $@"
    np=$1
    shift 1
    if echo "$@"|grep -q "incrementaltest"; then
        RDMAV_FORK_SAFE=1 PATH=build/:$PATH mpirun -np $np --oversubscribe build/mallob -pls=0 $@ 2>&1 > _systest &
        waitpids=$!
        for f in _incremental_jobs-* ; do
            run_incremental "$f" &
            newpid=$!
            waitpids="$newpid $waitpids"
        done
        for pid in $waitpids; do
            wait $pid
            echo "Wait for $pid done"
        done
    else
        RDMAV_FORK_SAFE=1 PATH=build/:$PATH mpirun -np $np --oversubscribe build/mallob $@ 2>&1 > _systest
        #RDMAV_FORK_SAFE=1 PATH=build/:$PATH mpirun -np $np --oversubscribe build/mallob $@ 2>&1 |grep -B10 -A10 "No such file"
    fi
}

function check() {
    echo "Checking ..."
    if grep -qi ERROR _systest ; then
        error "An error occurred during the execution."
    fi
    if grep -q "assertresult=SAT" _systest && ! grep -q "found result SAT" _systest ; then
        error "Expected result SAT was not found."
    fi
    if grep -q "assertresult=UNSAT" _systest && ! grep -q "found result UNSAT" _systest ; then
        error "Expected result UNSAT was not found."
    fi
    if grep -q "checkjsonresults" _systest; then
        cd .api/jobs.0/introduced
        for f in *.json; do
            if [ ! -f ../out/$f ]; then
                error "No result JSON reported for $f."
            fi
            if echo $f|grep -qi unsat ; then
                if ! grep -q '"resultstring": "UNSAT"' ../out/$f ; then
                    error "Expected result UNSAT for $f was not found."
                fi
            else
                if ! grep -q '"resultstring": "SAT"' ../out/$f ; then
                    error "Expected result SAT for $f was not found."
                fi
            fi
        done
        cd ../../..
    fi
    if [ '/dev/shm/edu.kit.iti.mallob*' != "$(echo /dev/shm/edu.kit.iti.mallob*)" ]; then
        error "Shared memory segment(s) not cleaned up: $(echo /dev/shm/edu.kit.iti.mallob*)"
    fi
}

function test() {
    echo "--------------------------------------------------------------------------------"
    run $@
    check $@
    testcount=$((testcount+1))
    if [ -z $nocleanup ]; then cleanup; fi
}

function introduce_job() {
    jobname=$1
    instance=$2
    if [ -z $wclimit ]; then wclimit="0"; fi
    if [ -z $arrival ]; then arrival="0"; fi
    if [ -z $dependency ]; then dependency=""; fi
    if [ -z $application ]; then application="SAT"; fi
    if [ -z $maxdemand ]; then maxdemand="0"; fi
    if [ -z $priority ]; then priority="1"; fi
    if [ -z "$appconfig" ]; then appconfig="{}"; fi
    echo '{ "application": "'$application'", "arrival": '$arrival', "dependencies": ['$dependency'], "user": "admin", "name": "'$jobname'", "files": ["'$instance'"], "priority": '$priority', "wallclock-limit": "'$wclimit'", "cpu-limit": "0", "max-demand": '$maxdemand', "configuration": '"$appconfig"' }' > .api/jobs.0/in/$1.json
    cp .api/jobs.0/in/$1.json .api/jobs.0/introduced/admin.$1.json
}

globalcount=1
function introduce_incremental_job() {
    jobname=${globalcount}-$1
    instance=instances/incremental/$1
    r=0
    last_revname=""
    while read -r result; do
        revname=${jobname}-${r}-$result
        cnfname=${instance}-${r}.cnf
        if [ $r == 0 ] ; then
            echo '{"cpu-limit": "0", "files": ["'$cnfname'"], "incremental": true, "application": "SAT",
            "name": "'$revname'", "priority": 1.0, "user": "admin",
            "wallclock-limit": "0"}' > .api/jobs.0/introduced/${revname}.json
        else
            echo '{"cpu-limit": "0", "files": ["'$cnfname'"], "incremental": true, "application": "SAT",
            "name": "'$revname'", "precursor": "admin.'$last_revname'", "priority": 1.0, "user": "admin",
            "wallclock-limit": "0"}' > .api/jobs.0/introduced/${revname}.json    
        fi
        r=$((r+1))
        last_revname=$revname
        echo ${revname}.json $result >> _incremental_jobs-$globalcount
    done < instances/incremental/$1

    revname=${jobname}-${r}-$result
    echo '{"cpu-limit": "0", "files": ["NONE"], "incremental": true, "done": true, "application": "SAT",
        "name": "'$revname'", "precursor": "admin.'$last_revname'", "priority": 1.0, "user": "admin",
        "wallclock-limit": "0"}' > .api/jobs.0/introduced/${revname}.json
    echo ${revname}.json >> _incremental_jobs-$globalcount
    
    globalcount=$((globalcount+1))
}

export -f cleanup
export -f error
export -f run_incremental
export -f run 
export -f check
export -f test
export -f introduce_job
export -f introduce_incremental_job

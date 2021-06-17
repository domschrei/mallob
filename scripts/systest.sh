#!/bin/bash

set -e

testcount=1

function cleanup() {
    set +e
    rm .api/jobs.0/*/*.json _systest _incremental_jobs /dev/shm/edu.kit.iti.mallob* 2> /dev/null
    set -e
}

function error() {
    echo "ERROR: $@"
    exit 1
}

function run() {
    echo "[$testcount] -np $@"
    np=$1
    shift 1
    if echo "$@"|grep -q "incrementaltest"; then
        RDMAV_FORK_SAFE=1 PATH=build/:$PATH mpirun -np $np --oversubscribe build/mallob $@ 2>&1 > _systest &
        mallobpid=$!
        while read line; do
            
            file=$(echo $line|awk '{print $1}')
            result=$(echo $line|awk '{print $2}')
            
            echo cp .api/jobs.0/{introduced,new}/$file
            cp .api/jobs.0/{introduced,new}/$file

            if [ "$result" == "" ] ; then
                break
            fi

            donefile=.api/jobs.0/done/admin.$file
            while [ ! -f $donefile ]; do sleep 0.1 ; done
            
            echo "$donefile present"
            
            if [ $result == unsat ] ; then
                if ! grep -q '"resultstring": "UNSAT"' $donefile ; then
                    error "Expected result UNSAT for $file was not found."
                fi
            elif [ $result == sat ] ; then
                if ! grep -q '"resultstring": "SAT"' $donefile ; then
                    error "Expected result SAT for $file was not found."
                fi
            fi
            rm $donefile
        done < _incremental_jobs
        wait $mallobpid
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
            if [ ! -f ../done/$f ]; then
                error "No result JSON reported for $f."
            fi
            if echo $f|grep -qi unsat ; then
                if ! grep -q '"resultstring": "UNSAT"' ../done/$f ; then
                    error "Expected result UNSAT for $f was not found."
                fi
            else
                if ! grep -q '"resultstring": "SAT"' ../done/$f ; then
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
    cleanup
}

function introduce_job() {
    jobname=$1
    instance=$2
    echo '{ "user": "admin", "name": "'$jobname'", "file": "'$instance'", "priority": 1.000, "wallclock-limit": "0", "cpu-limit": "0" }' > .api/jobs.0/new/$1.json
    cp .api/jobs.0/new/$1.json .api/jobs.0/introduced/admin.$1.json
}

function introduce_incremental_job() {
    jobname=$1
    instance=instances/incremental/$jobname
    r=0
    last_revname=""
    while read -r result; do
        revname=${jobname}-${r}-$result
        cnfname=${instance}-${r}.cnf
        if [ $r == 0 ] ; then
            echo '{"cpu-limit": "0", "file": "'$cnfname'", "incremental": true,
            "name": "'$revname'", "priority": 1.0, "user": "admin",
            "wallclock-limit": "0"}' > .api/jobs.0/introduced/${revname}.json
        else
            echo '{"cpu-limit": "0", "file": "'$cnfname'", "incremental": true,
            "name": "'$revname'", "precursor": "admin.'$last_revname'", "priority": 1.0, "user": "admin",
            "wallclock-limit": "0"}' > .api/jobs.0/introduced/${revname}.json    
        fi
        r=$((r+1))
        last_revname=$revname
        echo ${revname}.json $result >> _incremental_jobs
    done < instances/incremental/$jobname

    revname=${jobname}-${r}-$result
    echo '{"cpu-limit": "0", "file": "NONE", "incremental": true, "done": true,
        "name": "'$revname'", "precursor": "admin.'$last_revname'", "priority": 1.0, "user": "admin",
        "wallclock-limit": "0"}' > .api/jobs.0/introduced/${revname}.json
    echo ${revname}.json >> _incremental_jobs
}

mkdir -p .api/jobs.0/
mkdir -p .api/jobs.0/{introduced,new,pending,done}/
cleanup



# Basic mono tests

for mode in thread fork; do
    for slv in l g c lgc; do

        instancefile="instances/r3sat_300.cnf"
        test 1 -t=1 -mono=$instancefile -satsolver=$slv -appmode=$mode -v=4 -assertresult=SAT
        test 1 -t=8 -mono=$instancefile -satsolver=$slv -appmode=$mode -v=4 -assertresult=SAT
        test 8 -t=2 -mono=$instancefile -satsolver=$slv -appmode=$mode -v=4 -assertresult=SAT

        instancefile="instances/r3unsat_300.cnf"
        test 1 -t=1 -mono=$instancefile -satsolver=$slv -appmode=$mode -v=4 -assertresult=UNSAT
        test 1 -t=8 -mono=$instancefile -satsolver=$slv -appmode=$mode -v=4 -assertresult=UNSAT
        test 8 -t=2 -mono=$instancefile -satsolver=$slv -appmode=$mode -v=4 -assertresult=UNSAT
    done
done

# Scheduling tests

for lbc in 4 8; do
    for slv in l g c lgc; do
        # 8 jobs (4 SAT, 4 UNSAT)
        for c in {1..4}; do
            introduce_job sat-$c instances/r3sat_300.cnf
            introduce_job unsat-$c instances/r3unsat_300.cnf
        done
        test 10 -t=2 -lbc=$lbc -J=8 -l=1 -satsolver=$slv -v=4 -checkjsonresults
    done
done

# Incremental tests

for test in entertainment08 roverg10 transportg29 ; do
    for slv in g c l lg; do
        introduce_incremental_job $test 
        test 4 -t=2 -l=1 -satsolver=$slv -v=5 -J=1 -incrementaltest -checksums=1
    done
done

echo "All tests done."

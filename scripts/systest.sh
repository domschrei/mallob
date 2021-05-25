#!/bin/bash

set -e

testcount=1

function cleanup() {
    set +e ; rm .api/jobs.0/*/*.json 2> /dev/null ; set -e
}

function error() {
    echo "ERROR: $@"
    exit 1
}

function run() {
    echo "Running test #$testcount: -np $@"
    np=$1
    shift 1
    RDMAV_FORK_SAFE=1 PATH=build/:$PATH mpirun -np $np --oversubscribe build/mallob $@ 2>&1 > _systest
}

function check() {
    echo "Checking test #$testcount: -np $@"
    if grep -q ERROR _systest ; then
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
        echo "Shared memory segment not cleaned up: $(echo /dev/shm/edu.kit.iti.mallob*)"
    fi
}

function test() {
    echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
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

mkdir -p .api/jobs.0/
mkdir -p .api/jobs.0/{introduced,new,pending,done}/
cleanup



# Scheduling tests

for lbc in 1 4; do
    # 8 jobs (4 SAT, 4 UNSAT)
    for c in {1..4}; do
        introduce_job sat-$c instances/r3sat_300.cnf
        introduce_job unsat-$c instances/r3unsat_300.cnf
    done
    test 10 -t=2 -lbc=$lbc -J=8 -l=1 -satsolver=l -v=4 -checkjsonresults
done



# Basic mono tests

for mode in thread fork; do

    instancefile="instances/r3sat_300.cnf"
    test 1 -t=1 -mono=$instancefile -satsolver=l -appmode=$mode -v=4 -assertresult=SAT
    test 1 -t=8 -mono=$instancefile -satsolver=l -appmode=$mode -v=4 -assertresult=SAT
    test 8 -t=2 -mono=$instancefile -satsolver=l -appmode=$mode -v=4 -assertresult=SAT

    instancefile="instances/r3unsat_300.cnf"
    test 1 -t=1 -mono=$instancefile -satsolver=l -appmode=$mode -v=4 -assertresult=UNSAT
    test 1 -t=8 -mono=$instancefile -satsolver=l -appmode=$mode -v=4 -assertresult=UNSAT
    test 8 -t=2 -mono=$instancefile -satsolver=l -appmode=$mode -v=4 -assertresult=UNSAT
done

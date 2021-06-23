#!/bin/bash

set -e

testcount=1
source $(dirname "$0")/systest_commons.sh

mkdir -p .api/jobs.0/
mkdir -p .api/jobs.0/{introduced,new,pending,done}/
cleanup

function test_mono() {
    for mode in thread fork; do
        for slv in lgc; do

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
}

function test_scheduling() {
    for lbc in 4 8; do
        for slv in lgc; do
            # 8 jobs (4 SAT, 4 UNSAT)
            for c in {1..4}; do
                introduce_job sat-$c instances/r3sat_300.cnf
                introduce_job unsat-$c instances/r3unsat_300.cnf
            done
            test 10 -t=2 -lbc=$lbc -J=8 -l=1 -satsolver=$slv -v=5 -checkjsonresults
        done
    done
}

function test_incremental() {
    for test in entertainment08 roverg10 transportg29 ; do
        for slv in lgc LgC; do
            introduce_incremental_job $test 
            test 4 -t=2 -l=1 -satsolver=$slv -v=5 -J=1 -incrementaltest -checksums=1
        done
    done
}

function test_oscillating() {
    # Generate periodic "disturbance" jobs
    t=4
    n=0
    while [ $t -le 60 ]; do
        # wallclock limit of 4s, arrival @ t
        introduce_job sat-$t instances/r3sat_300.cnf 4 $t
        t=$((t+8))
        n=$((n+1))
    done
    # Generate actual job
    introduce_job sat-main instances/r3sat_500.cnf 60
    test 13 -t=1 -lbc=2 -J=$((n+1)) -l=1 -satsolver=l -v=4 -checkjsonresults -checksums=1
}

test_oscillating
test_scheduling
#test_mono
#test_incremental

echo "All tests done."

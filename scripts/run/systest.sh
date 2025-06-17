#!/bin/bash

set -e

testcount=1
#MPIOPTS="--oversubscribe"

source $(dirname "$0")/systest_commons.sh

mkdir -p .api/jobs.0/
mkdir -p .api/jobs.0/{in,out,introduced}/
cleanup

glucose=""
if [ x$GLUCOSE == x1 ]; then
    glucose="g"
fi

function test_mono() {
    for slv in kcl${glucose} ; do

        instancefile="instances/r3unsat_250.cnf"
        test 16 -t=1 -mono=$instancefile -satsolver=$slv -assertresult=UNSAT -s=0.05 $@

        instancefile="instances/r3sat_300.cnf"
        test 1 -t=1 -mono=$instancefile -satsolver=$slv -assertresult=SAT $@
        test 1 -t=4 -mono=$instancefile -satsolver=$slv -assertresult=SAT $@
        test 4 -t=2 -mono=$instancefile -satsolver=$slv -assertresult=SAT $@

        instancefile="instances/r3unsat_300.cnf"
        test 1 -t=1 -mono=$instancefile -satsolver=$slv -assertresult=UNSAT $@
        test 1 -t=4 -mono=$instancefile -satsolver=$slv -assertresult=UNSAT $@
        test 4 -t=2 -mono=$instancefile -satsolver=$slv -assertresult=UNSAT $@
    done
}

function test_scheduling() {
    for lbc in 4 8; do
        for slv in l${glucose}ck; do
            # 8 jobs (4 SAT, 4 UNSAT)
            for c in {1..4}; do
                introduce_job sat-$c instances/r3sat_300.cnf
                introduce_job unsat-$c instances/r3unsat_300.cnf
            done
            test 10 -c=1 -t=2 -ajpc=$lbc -J=8 -satsolver=$slv -checkjsonresults $@
        done
    done
}

function test_dry_scheduling() {
    t=1
    for i in {1..400}; do
        # wallclock limit, arrival, dependencies, application
        wclimit=1.5s arrival=$t application=DUMMY introduce_job dummy-$i instances/r3sat_300.cnf
        t=$(echo "$t+0.1"|bc -l)
    done
    echo "400 jobs set up."
    test 32 -c=1 -J=400 $@
}

function test_job_streamer() {
    > .job-desc-template
    for f in instances/r3{sat,unsat}_{200,300}.cnf; do
        echo $f >> .job-desc-template
    done
    echo '{"priority":{"type":"constant","params":[1]},"maxdemand":{"type":"constant","params":[0]},"wallclock-limit":{"type":"constant","params":[10]},"arrival":{"type":"constant","params":[0]},"burstsize":{"type":"constant","params":[1]}}' > .client-template

    test 16 -v=4 -t=1 -J=60 -ajpc=3 -ljpc=6 -job-template=templates/job-template.json \
    -client-template=.client-template -job-desc-template=.job-desc-template $@
}

function test_incremental() {
    for test in entertainment08 roverg10 transportg29 ; do
        introduce_incremental_job $test 
        test 2 -c=1 -t=2 -satsolver=CL -J=1 -incrementaltest $@
    done
}

function test_many_incremental() {
    for i in {1..10}; do
        introduce_incremental_job entertainment08
    done
    test 4 -c=1 -t=2 -satsolver=CL -J=10 -ajpc=1 -incrementaltest $@
}

function test_oscillating() {
    # Generate periodic "disturbance" jobs
    t=4
    n=0
    RANDOM=1
    app=SAT
    while [ $t -le 60 ]; do
        # wallclock limit of 4s, arrival @ t
        wclimit=4s arrival=$t application=$app \
        maxdemand=$(($RANDOM % 7 + 1)) introduce_job disturb-$t instances/r3unknown_10k.cnf
        t=$((t+8))
        n=$((n+1))
    done
    # Generate actual jobs
    arrival=0 application=$app priority=0.4 introduce_job unsat-main-1 instances/r3unsat_300.cnf
    arrival=15 application=$app priority=0.3 introduce_job unsat-main-2 instances/r3unsat_300.cnf
    arrival=30 application=$app priority=0.2 introduce_job sat-main-3 instances/r3sat_300.cnf
    arrival=45 application=$app priority=0.1 introduce_job sat-main-4 instances/r3sat_300.cnf
    test 16 -t=1 -c=1 -J=$((n+4)) -satsolver=kcl${glucose} -checkjsonresults $@
}

function test_oscillating_ontheflycheck() {
    if [ ! -f build/impcheck_parse -o ! -f build/impcheck_check -o ! -f build/impcheck_confirm ]; then
        error "ImpCheck executable(s) not present at ./build/ - cannot run this test."
    fi

    # Generate periodic "disturbance" jobs
    t=4
    n=0
    RANDOM=1
    app=SAT
    while [ $t -le 90 ]; do
        # wallclock limit of 4s, arrival @ t
        wclimit=4s arrival=$t application=$app \
        maxdemand=$(($RANDOM % 7 + 1)) introduce_job disturb-$t instances/r3unknown_10k.cnf
        t=$((t+8))
        n=$((n+1))
    done
    # Generate actual jobs
    arrival=0 application=$app priority=0.4 introduce_job unsat-main-1 instances/r3unsat_300.cnf
    arrival=15 application=$app priority=0.3 introduce_job unsat-main-2 instances/r3unsat_300.cnf
    arrival=30 application=$app priority=0.2 introduce_job sat-main-3 instances/r3sat_300.cnf
    arrival=45 application=$app priority=0.1 introduce_job sat-main-4 instances/r3sat_300.cnf
    test 16 -t=1 -c=1 -J=$((n+4)) -satsolver='ck+(c)*' -checkjsonresults -otfc=1 -jc=2 $@
}

function test_incremental_scheduling() {
    for test in entertainment08 roverg10 transportg29 towers05 ; do
        introduce_incremental_job $test
    done
    test 8 -t=1 -c=1 -satsolver=CL -J=4 -incrementaltest $@
}

function test_certified_unsat() {
    test_cert_unsat 2 instances/r3unsat_200.cnf -assertresult=UNSAT $@
    test_cert_unsat 2 instances/r3sat_200.cnf -assertresult=SAT $@
    test_cert_unsat 8 instances/r3unsat_250.cnf -assertresult=UNSAT -t=2 $@
}

function test_ontheflycheck() {
    if [ ! -f build/impcheck_parse -o ! -f build/impcheck_check -o ! -f build/impcheck_confirm ]; then
        error "ImpCheck executable(s) not present at ./build/ - cannot run this test."
    fi

    instancefile="instances/r3sat_200.cnf"
    test 1 -t=1 -mono=$instancefile -otfc=1 -assertresult=VSAT $@
    test 1 -t=8 -mono=$instancefile -otfc=1 -satsolver='ck+l+(c)*' -assertresult=VSAT $@
    test 4 -t=4 -mono=$instancefile -otfc=1 -satsolver='ck+l+(c)*' -assertresult=VSAT $@
    
    for instancefile in instances/r3unsat_{2,3}00.cnf ; do
        test 1 -t=1 -mono=$instancefile -otfc=1 -assertresult=VUNSAT $@
        test 1 -t=8 -mono=$instancefile -otfc=1 -satsolver='ck+l+(c)*' -assertresult=VUNSAT $@
        test 4 -t=4 -mono=$instancefile -otfc=1 -satsolver='ck+l+(c)*' -assertresult=VUNSAT $@
    done
}


if [ -z "$1" ] || [ "$1" == "-h" ] || [ "$1" == "--help" ]; then
    echo "Usage: [nocleanup=1] $0 [<mallob-option-overrides>] <test case> [<more test cases> ...]"
    echo "Possible test cases: mono drysched sched osc stream inc manyinc incsched certunsat ontheflycheck oscontheflycheck all"
    exit 0
fi

progopts=""
while [ ! -z "$1" ]; do
    arg="$1"
    case $arg in
        all)
            test_mono $progopts
            test_dry_scheduling $progopts
            test_scheduling $progopts
            test_oscillating $progopts
            test_job_streamer $progopts
            test_incremental $progopts
            test_many_incremental $progopts
            test_incremental_scheduling $progopts
            test_certified_unsat $progopts
            test_ontheflycheck $progopts
            test_oscillating_ontheflycheck $progopts
            ;;
        mono)
            test_mono $progopts
            ;;
        drysched)
            test_dry_scheduling $progopts
            ;;
        sched)
            test_scheduling $progopts
            ;;
        osc)
            test_oscillating $progopts
            ;;
        oscontheflycheck)
            test_oscillating_ontheflycheck $progopts
            ;;
        stream)
            test_job_streamer $progopts
            ;;
        inc)
            test_incremental $progopts
            ;;
        manyinc)
            test_many_incremental $progopts
            ;;
        incsched)
            test_incremental_scheduling $progopts
            ;;
        certunsat)
            test_certified_unsat $progopts
            ;;
        ontheflycheck)
            test_ontheflycheck $progopts
            ;;
        -*)
            print_separator
            echo "Adding program option \"$arg\""
            progopts="$progopts $arg"
            ;;
        *)
            print_separator
            echo "Unknown argument $1"
            exit 1
    esac
    shift 1
done

echo "All tests done."

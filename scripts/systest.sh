#!/bin/bash

function error() {
    echo "ERROR: $@"
    exit 1
}

function run() {
    echo "Running test: $@"
    np=$1
    shift 1
    RDMAV_FORK_SAFE=1 PATH=build/:$PATH mpirun -np $np --oversubscribe build/mallob $@ 2>&1 > _systest
}

function check() {
    echo "Checking test: $@"
    if grep -q ERROR _systest ; then
        error "An error occurred during the execution."
    fi
    if grep -q "assertresult=SAT" _systest && ! grep -q "found result SAT" _systest ; then
        error "Expected result SAT was not found."
    fi
    if grep -q "assertresult=UNSAT" _systest && ! grep -q "found result UNSAT" _systest ; then
        error "Expected result UNSAT was not found."
    fi
}

function test() {
    echo "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
    run $@
    check $@
}

instancefile="instances/r3sat_200.cnf"
test 1 -t=1 -mono=$instancefile -satsolver=l -appmode=thread -v=4 -assertresult=SAT
test 1 -t=8 -mono=$instancefile -satsolver=l -appmode=thread -v=4 -assertresult=SAT
test 8 -t=2 -mono=$instancefile -satsolver=l -appmode=thread -v=4 -assertresult=SAT

instancefile="instances/r3unsat_200.cnf"
test 1 -t=1 -mono=$instancefile -satsolver=l -appmode=thread -v=4 -assertresult=UNSAT
test 1 -t=8 -mono=$instancefile -satsolver=l -appmode=thread -v=4 -assertresult=UNSAT
test 8 -t=2 -mono=$instancefile -satsolver=l -appmode=thread -v=4 -assertresult=UNSAT

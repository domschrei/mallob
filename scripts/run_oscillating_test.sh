#!/bin/bash

set -e

testcount=1
source $(dirname "$0")/systest_commons.sh

mkdir -p .api/jobs.0/
mkdir -p .api/jobs.0/{introduced,new,pending,done}/
cleanup

# Generate periodic "disturbance" jobs
t=5
n=0
job_before="admin.disturb-$t"
while [ $t -le 24000 ]; do
    # wallclock limit of 15s, arrival @ t
    introduce_job disturb-$t instances/r3unknown_100k.cnf 15 $t
    t=$((t+30))
    n=$((n+1))
done
# Generate actual jobs
for i in {1..80}; do
    job_before=\"$(introduce_job solve-$i instances/$(cat .api/benchmark_sat2020_selection_dec|head -$i|tail -1) 300 0 $job_before)\"
    echo $job_before
done

RDMAV_FORK_SAFE=1 PATH=build/:$PATH mpirun -np $1 --oversubscribe build/mallob \
-t=4 -l=1 -g=0 -satsolver=l -v=4 -T=24000 -ch=1 -chaf=5 -chstms=60 -appmode=fork \
-cfhl=1 -smcl=0 -hmcl=0 -ihlbd=0 -islbd=0 -fhlbd=0 -fslbd=0 -checksums=1 -log=test_$$

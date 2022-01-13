#!/bin/bash

set -e

echo "Mallob mono"

#RESULTS/mallob_mono_*/
for d in RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_all_n128/ ; do
    for i in {1..400}; do
        if [ -f $d/mallob_mono_*_i${i}_log ]; then
            scripts/check_results.sh $d/mallob_mono_*_i${i}_log
        fi
    done
done

exit

echo "Mallob scheduled"

for d in RESULTS/mallob_sched_uniform*/ ; do
    if [ -f $d/mallob_sched_*_log ]; then
        scripts/check_results.sh $d/mallob_sched_*_log
    elif [ -f $d/OUT ]; then
        scripts/check_results.sh $d/OUT
    else
        echo "WARN: $d could not be verified!"
    fi
done

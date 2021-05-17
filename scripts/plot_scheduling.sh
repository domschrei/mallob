#!/bin/bash

set -e

# RESULTS/mallob_sched_osc_cfhl0_2 RESULTS/mallob_sched_osc_cfhl10 RESULTS/mallob_sched_osc_cfhl30_2 RESULTS/mallob_sched_osc_cfhl90_2
for d in RESULTS/mallob_sched_osc_cfhl0_2 RESULTS/mallob_sched_osc_cfhl0_big ; do
    
    ncomputenodes=64
    ncorespernode=24
    timeout=300
    njobs=80
    
    infile=$d/OUT
 
    # Create a mapping between local and global job IDs

    # Create ID -> instance map
    cat $infile |grep -oE "[0-9\.]+ [0-9]+ <Reader> \[T\] (Reading|Initialized) job .*"|awk '{print $7,$8}'|sed 's/^#//g'|sed 's/[()]//g'|sort -u|sort -g|head -$njobs > $d/id_map
    wc -l $d/id_map
    #cat $d/id_map
    
    # Find number of jobs
    #njobs=$(cat $infile|grep -oE "#[0-9]+ "|grep -oE "[0-9]+"|awk 'BEGIN {max=0} {max=$1>max?$1:max} END {print max}')
    #echo $njobs jobs
    
    # Create global to local ID map
    for i in $(seq 1 $njobs); do 
        inst=$(head -$i .api/benchmark_sat2020_forhlr|tail -1|sed 's/\.xz$//g')
        echo $i $(cat $d/id_map|grep "$inst"|awk '{print $1}')
    done > $d/global_to_local_id
    wc -l $d/global_to_local_id

    # Create local to global ID map
    cat $d/global_to_local_id|awk '{print $2,$1}'|sort -g > $d/local_to_global_id
    wc -l $d/local_to_global_id
    

    # Extract runtimes
    
    cat $infile|grep -oE "[0-9\.]+ [0-9]+ RESPONSE_TIME #[0-9]+ [0-9\.]+"|awk '{print $4,"xxx",$5}'|sed 's/#//g'|while read -r line; do
        localid=$(echo $line|awk '{print $1}')
        globalid=$(grep -E "^$localid " $d/local_to_global_id|awk '{print $2}')
        echo $globalid $(echo $line|awk '{print $2,$3}')
    done > $d/runtimes
    cat $infile|grep -oE "[0-9\.]+ [0-9]+ RESPONSE_TIME #[0-9]+ [0-9\.]+"|awk '{print $4,"xxx",$1}'|sed 's/#//g'|while read -r line; do
        localid=$(echo $line|awk '{print $1}')
        globalid=$(grep -E "^$localid " $d/local_to_global_id|awk '{print $2}')
        echo $globalid $(echo $line|awk '{print $2,$3}')
    done > $d/donetimes
    cat $d/runtimes|awk '{print $3}'|sort -g|awk '{print $1,NR}' > $d/cdf_runtimes
    cat $d/donetimes|awk '{print $3}'|sort -g|awk '{print $1,NR}' > $d/cdf_donetimes
        
    
    # Solved statistics and PAR2 scores
    
    solved=0
    solved_sat=0
    solved_unsat=0
    score=0
    while read -r line; do
        
        echo $line
        
        localid=$(echo $line|awk '{print $1}')
        globalid=$(head -$localid $d/local_to_global_id|tail -1|awk '{print $2}')
        
        runtime=$(cat $d/runtimes|awk '$1 == '$globalid' {print $3}')
        solved=$((solved+1))
        if echo $line|grep -q UNSAT; then solved_unsat=$((solved_unsat+1))
        else solved_sat=$((solved_sat+1)); fi
        score=$(echo "$score + $runtime"|bc -l)
        
        #echo $globalid $runtime $score $solved $solved_sat $solved_unsat
        
    done < <(cat $infile|grep -oE "[0-9]+ SOLUTION #[0-9]+ (UN)?SAT"|awk '{print $3,$4}'|sed 's/#//g')
    
    score=$(echo "$score + ($njobs-$solved) * 2 * $timeout"|bc -l)
    score=$(echo "$score / $njobs"|bc -l)
    echo "$test $d : PAR2=$score solved=$solved SAT=$solved_sat UNSAT=$solved_unsat"
    echo $score > $d/par2score
    echo $solved_sat > $d/solved_sat
    echo $solved_unsat > $d/solved_unsat
    
    continue
    
    # Job scheduling times
    cat $infile|grep -E "^[0-9\.]+ [0-9]+ "|grep -E "Introducing|Sending job desc" > $d/raw_scheduling_events
    > $d/scheduling_times
    all_durations=0
    c=0
    for globj in $(seq 1 $njobs); do
        locj=$(head -$globj $d/global_to_local_id|tail -1|awk '{print $2}')
        introtime=$(cat $d/raw_scheduling_events|grep "Introducing job #$locj "|awk '{print $1}')
        schedtime=$(cat $d/raw_scheduling_events|grep "Sending job desc. of #$locj "|awk '{print $1}')
        echo $introtime $schedtime
        duration=$(echo "$schedtime - $introtime"|bc -l)
        echo $globj $duration >> $d/scheduling_times
        all_durations=$(echo "$all_durations + $duration"|bc -l)
        c=$((c+1))
    done
    sort -g $d/scheduling_times -o $d/scheduling_times
    minavgmax=$(cat $d/scheduling_times|awk 'BEGIN {min=100; max=0} $2 < min {min=$2} $2 > max {max=$2} {c+=1; s+=$2} END {print min, s/c, max}')
    median=$(cat $d/scheduling_times|awk '{print $2,$1}'|sort -g|awk 'NR == int('$(cat $d/scheduling_times|wc -l)'/2) {print $1}')
    echo "Scheduling times : min/avg/max $minavgmax median $median"

    # Overall volume and #jobs
    cat $infile|grep "sysstate"|awk '{print $1,$6,$7}'|sed 's/[a-z]\+=//g' > $d/scheduled_and_done_jobs
    cat $d/scheduled_and_done_jobs|awk '{print $1,$2}' > $d/scheduled_jobs
    cat $d/scheduled_and_done_jobs|awk '{print $1,$3}' > $d/done_jobs
    cat $d/*_log|grep -oE "^[0-9\.]+ 0 sysstate.*globmem"|awk '{print $1,$4,$5}'|sed 's/[a-z]\+=//g' > $d/load_and_jobs
    cat $d/load_and_jobs|awk '{print $1,$2}' > $d/load
    cat $d/load_and_jobs|awk '{print $1,$3}' > $d/jobs
    
    #continue
    
    # Migrations
    J=$(cat $infile|grep "Program options"|head -1|grep -oE "lbc=[0-9]+"|grep -oE "[0-9]+")
    #base_adoptions=$(echo "80 * (485/$J)"|bc -l)
    #adoptions=$(cat $d/*_log|grep ADOPT|wc -l)
    #readoptions=$(cat $d/*_log|grep ADOPT|grep oneshot=1|wc -l)
    #rejections=$(cat $d/*_log|grep REJECT|wc -l)
    cat $d/*_log|grep "update v=" > $d/raw_volume_updates
    base_adoptions=0
    for j in $(seq 1 $njobs); do
        base_adoptions=$(($base_adoptions + $(cat $d/raw_volume_updates|grep "#${j}:0"|sed 's/v=//g'|awk 'BEGIN {max=0} $6 > max {max=$6} END {print max}')))
    done
    transfers=$(cat $d/*_log|grep "Sent job desc."|wc -l)
    starts=$(cat $d/*_log|grep START|wc -l)
    echo base adoptions: $base_adoptions, transfers: $transfers, starts: $starts
        
    # CPU usage
    cat $d/*_log|grep -E "mainthread"|grep -oE "cpuratio=[0-9\.]+"|grep -oE "[0-9\.]+"|awk '{c += 1; s += $1} END {print s/c}'
    cat $d/*_log|grep -E "child_main"|grep -oE "cpuratio=[0-9\.]+"|grep -oE "[0-9\.]+"|awk '{c += 1; s += $1} END {print s/c}'
    cat $d/*_log|grep cpuratio|grep -E "td\."|grep -oE "cpuratio=[0-9\.]+"|grep -oE "[0-9\.]+"|awk '{c += 1; s += ($1>1?1:$1)} END {print s/c}'
        
    # Load events
    grep LOAD $d/*_log|grep -oE "^[0-9]+\.[0-9]+ [0-9]+ LOAD [01] \([+-]#[0-9]+:[0-9]+\)"|awk '{print $1,$2,$4,$5}'|sed 's.(\|)\|\+\|-\|#..g'|sed 's/:/ /g' > $d/loadevents
    sort -g $d/loadevents -o $d/loadevents
    python3 process_job_volumes.py $d > $d/cpu_seconds_per_job
    cat $d/cpu_seconds_per_job|awk '{print $3}'|sort -g|awk '{print $1,NR}' > $d/cdf_cpu_seconds
    
    # Extract total resources used
    n=$ncomputenodes
    cpus=$(($n * $ncorespernode))
    time=$(cat $d/*_log|grep "Exiting happily"|tail -1|awk '{print $1}')
    total=$(echo "$cpus * $time"|bc -l)
    echo $total > $d/cpu_seconds_total
    echo "$n nodes, $cpus CPUs, $time seconds => $total CPU seconds"
    echo "$d : $total CPU seconds"
    
done

exit


python3 scripts/plot_curves.py -xy RESULTS/mallob_sched_burst_cfhl90_2/load -l="Load" -xsize=5.5 -ysize=2 -xmin=0 -xmax=90 -ymin=0.7 -ymax=1 -nomarkers -nolinestyles -nolegend -ylabel="System load" -o=sched400_load.pdf

python3 scripts/plot_curves.py -xy RESULTS/mallob_sched_burst_cfhl90_2/scheduled_jobs -l="Scheduled" RESULTS/mallob_sched_burst_cfhl90_2/done_jobs -l="Done" RESULTS/mallob_sched_burst_cfhl90_2/jobs -l="Active" -xsize=5.5 -ysize=2 -xmin=0 -xmax=90 -ymin=0 -ymax=400 -nomarkers -xlabel="Time / s" -ylabel="\# jobs" -o=sched400_jobs.pdf

cp sched*_*.pdf ../paper_sat2021_mallob/

exit

# Total job scheduling times
minavgmax=$(cat RESULTS/*/scheduling_times|awk 'BEGIN {min=1; max=0} $2 < min {min=$2} $2 > max {max=$2} {c+=1; s+=$2} END {print min, s/c, max}')
median=$(cat RESULTS/*/scheduling_times|awk '{print $2,$1}'|sort -g|awk 'NR == int('$(cat $d/scheduling_times|wc -l)'/2) {print $1}')
echo "Total scheduling times : min/avg/max $minavgmax median $median"

python3 scripts/plot_curves.py -xy RESULTS/mallob_sched_uniform16_g0_cfhl90/scheduled_jobs -l="Scheduled" RESULTS/mallob_sched_uniform16_g0_cfhl90/done_jobs -l="Done" RESULTS/mallob_sched_uniform16_g0_cfhl90/jobs -l="Active" -xsize=4 -ysize=2 -xmin=0 -xmax=845 -ymin=0 -nomarkers -xlabel="Time / s" -ylabel="\# jobs" -o=sched16_jobs.pdf
cp sched*_*.pdf ../paper_sat2021_mallob/

python3 scripts/plot_curves.py -xy RESULTS/mallob_sched_uniform16_g0_cfhl90/load -l="Load" -xsize=5.5 -ysize=2 -xmin=0 -xmax=845 -ymin=0.7 -ymax=1 -nomarkers -nolinestyles -nolegend -ylabel="System load" -o=sched16_load.pdf




# Compute total resources used for mono baseline
#if false; then
n=128
# RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n{128,32,8}
for d in RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n{128,32,8,2} ; do
    cpus=$(($n * 20))
    total=0
    c=0
    > $d/cpu_seconds_per_job
    for i in {1..80}; do
        time=$(tail -n 4000 $d/*_i${i}_log|grep -oE "^c [0-9\.]+ [0-9]+ "|tail -1|awk '{print $2}')
        r=$(echo "$cpus * $time"|bc -l)
        total=$(echo "$total + $r"|bc -l)
        c=$((c+1))
        echo $i : $r >> $d/cpu_seconds_per_job
    done
    #while read -r line; do
    #    time=$(echo $line|awk '{print $3}')
    #done < $d/runtimes
    #total=$(echo "$total + (80-$c) * $cpus * 300"|bc -l)
    echo $total > $d/cpu_seconds_total
    echo "$d : $total CPU seconds"
    cat $d/cpu_seconds_per_job|awk '{print $3}'|sort -g|awk '{print $1,NR}' > $d/cdf_cpu_seconds
    n=$((n/4))
done
#fi

python3 scripts/plot_curves.py -xy \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n32/cdf_cpu_seconds  -l='mono n=32' \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n8/cdf_cpu_seconds   -l='mono n=8' \
RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n2/cdf_cpu_seconds   -l='mono n=2' \
RESULTS/mallob_sched_uniform4_g0_cfhl90/cdf_cpu_seconds        -l='sched J=4' \
RESULTS/mallob_sched_uniform16_g0_cfhl90/cdf_cpu_seconds       -l='sched J=16' \
RESULTS/mallob_sched_uniform64_g0_cfhl90/cdf_cpu_seconds       -l='sched J=64' \
-xlabel="CPU time / s" -ylabel="\\# instances solved in $\\leq t$ CPUs"


d1=RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n2
d2=RESULTS/mallob_sched_uniform64_g0_cfhl90
python3 scripts/plot_1v1.py $d1/runtimes -l="$d1" $d2/runtimes -l="$d2" -T=300

d1=RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n2
d2=RESULTS/mallob_sched_uniform64_g0_cfhl90
python3 scripts/plot_1v1.py $d1/cpu_seconds_per_job -l="$d1" $d2/cpu_seconds_per_job -l="$d2" -T=1000000

exit 0

d1=RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n32
d2=RESULTS/mallob_sched_uniform4_g0_cfhl90
python3 scripts/plot_1v1.py $d1/runtimes -l="$d1" $d2/runtimes -l="$d2" -T=300

d1=RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n128
d2=RESULTS/mallob_sched_uniform4_g0_cfhl90
python3 scripts/plot_1v1.py $d1/runtimes -l="$d1" $d2/runtimes -l="$d2" -T=300

d1=RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n32
d2=RESULTS/mallob_sched_uniform4_g0_cfhl90
python3 scripts/plot_1v1.py $d1/cpu_seconds_per_job -l="$d1" $d2/cpu_seconds_per_job -l="$d2" -T=1000000

d1=RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n8
d2=RESULTS/mallob_sched_uniform16_g0_cfhl90
python3 scripts/plot_1v1.py $d1/runtimes -l="$d1" $d2/runtimes -l="$d2" -T=300

d1=RESULTS/mallob_mono_cbdf0.875_mlbd00_mcl0_n8
d2=RESULTS/mallob_sched_uniform16_g0_cfhl90
python3 scripts/plot_1v1.py $d1/cpu_seconds_per_job -l="$d1" $d2/cpu_seconds_per_job -l="$d2" -T=1000000


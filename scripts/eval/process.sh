#!/bin/bash

if [ -z $1 ]; then
    echo "Provide a log directory."
    exit 1
fi

basedir=$(pwd)

logdir="$1"
cd "$logdir"

# Get number of machines (MPI processes)
m=0
while [ -d $m ]; do
    m=$((m+1))
done

rm *_history_#* _assignment_history load_history loadevents 2>/dev/null

# For each MPI process
for d in $(seq 0 $(($m-1))); do
    
    echo $d
    
    # Clean and repair logs merged incompletely
    if ls $d/|grep -q "#"; then
        cat $d/jobs.$d "$d/log.${d}#"* |sort -g > _jobs
        rm $d/log.${d}#*
        mv _jobs $d/jobs.$d
    fi
    # Result directory
    if [ -d $d/res ]; then rm -rf $d/res ; fi
    mkdir $d/res
    
    if [ "$d" == "0" ]; then
        # System state
        # 728.439 0 sysstate busyratio=0.937 jobs=59 globmem=46.46GB
        grep sysstate $d/log.$d|awk '{print $1,$4,$5,$6}'|sed 's/[[:alpha:]=]//g' > _sysstate
        awk '{print $1,$2}' _sysstate > $d/res/load
        awk '{print $1,$3}' _sysstate > $d/res/num_jobs
        awk '{print $1,$4}' _sysstate > $d/res/accumulated_memory
        rm _sysstate
        
        # Balancing
        grep -E "BLC.*DONE" $d/log.$d|awk '{print $1,$9}'|sed 's/[[:alpha:]=]//g' >> $d/res/balancer_utilization
        loadfactor=$(grep -E "Program options" $d/log.$d|grep -oE " l=[0-9\.]+"|grep -oE "[0-9\.]+")
        clients=$(grep -E "Program options" $d/log.$d|grep -oE " c=[0-9]+"|grep -oE "[0-9]+")
        workers=$((m-clients))
        aimed_load=$(echo "$loadfactor * $workers"|bc -l)
        grep -E "BLC.*DONE" $d/log.$d|sed 's/[[:alpha:]]\+=//g'|awk '{print $1,$9/'$workers'}' >> $d/res/balancer_loadfactor
        
        # Volume assignments
        grep -E "BLC assigned" $d/log.$d|while read -r line; do
            time=$(echo $line|awk '{print $1}')
            echo $line|grep -oE "#[0-9]+:[0-9]+"|sed 's/#//g'|sed 's/:/ /g'|awk '{print $1,"'$time'",$2}' >> "_assignment_history"
        done
        jobs=$(awk '{print $1}' _assignment_history|sort -u)
        while read -r jobid ; do
            grep -E "^$jobid " _assignment_history|awk '{print $2,$3}' > assignment_history_#$jobid
        done <<< $jobs
    fi
    
    # CPU usage
    # 39.361 9 mainthread cpuratio=0.020 sys=0.667
    grep -oE "^[0-9\.]+ $d mainthread cpuratio=[0-9\.]+ sys=[0-9\.]+" $d/log.$d|sed 's/[[:alpha:]]\+=//g'|awk '{print $1,$4,$5}' > _cpu
    cat _cpu|awk '{print $1,$2}' >> $d/res/mainthread_cpuratio
    cat _cpu|awk '{print $1,$3}' >> $d/res/mainthread_sysratio
    rm _cpu
    
    # Load events
    grep LOAD $d/log.$d|grep -oE "^[0-9]+\.[0-9]+ $d LOAD [01] \([+-]#[0-9]+:[0-9]+\)"|awk '{print $1,$2,$4,$5}'|sed 's.(\|)\|\+\|-\|#..g'|sed 's/:/ /g' >> $d/res/loadevents
    
    # Client events
    grep -oE "^[0-9\.]+ $d Introducing" $d/log.$d|awk '{print $1,NR}' >> $d/res/num_introduced_jobs
    grep -oE "^[0-9\.]+ $d (RESPONSE_TIME|TIMEOUT)" $d/log.$d|awk '{print $1,NR}' >> $d/res/num_exited_jobs
    
    # Job volume updates
    #vollines=$(grep -oE "^[0-9]+\.[0-9]+ $d #[0-9]+:0 : update v=[0-9]+" $d/log.$d)
    #while read -r line; do
    #    time=$(echo $line|awk '{print $1}')
    #    jobid=$(echo $line|grep -oE "#[0-9]+"|grep -oE "[0-9]+")
    #    volume=$(echo $line|grep -oE "v=[0-9]+"|grep -oE "[0-9]+")
    #    echo "$time $volume" >> "assignment_history_#$jobid"
    #done <<< $vollines
    
done

cat */res/loadevents|awk '{print $1,$2,$3,$4,$5}'|sort -s -g > loadevents

python3 $basedir/process_job_volumes.py .

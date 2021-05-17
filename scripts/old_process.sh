#!/bin/bash

function flatten_logs() {

    n=0
    while [ -d "$logdir/$n/" ]; do
        for file in $logdir/$n/{jobs,log}.*; do
            # Format butchered log lines to contain the most recent
            # correctly formatted timestamp
            awk 'BEGIN {time="0.000"} \
            !/^[0-9]+\.[0-9]+ / {print time,$0} \
            /^[0-9]+\.[0-9]+ / {time=$1; print $0}' $file > ${file}_CAT
        done
        cat $logdir/$n/*_CAT |sort -g > $logdir/log_cat.$n
        rm $logdir/$n/*_CAT
        n=$((n+1))
    done
}

function get_client_nodes() {

    num_nodes=`ls $logdir/log_*.*|wc -l`
    num_clients=`head $logdir/log_*.0|grep -m1 "Program options"|grep -oE ", c=[0-9]+,"|grep -oE "[0-9]+"`
    startnode=$(($num_nodes-$num_clients))
    endnode=$(($num_nodes-1))
    seq $startnode $endnode
}

function extract_client_info() {

    > $logdir/responses
    > $logdir/timeouts
    > $logdir/runtimes
    > $logdir/qualified_runtimes
    > $logdir/scheduling_times

    introduced_jobs=0
    for n in $(get_client_nodes); do

        client_logfile=$logdir/log_*.$n
        echo $client_logfile
        
        introduced=`grep "Introducing job #" $client_logfile|wc -l`
        cat $client_logfile|grep "RESPONSE_TIME" >> $logdir/responses
        cat $client_logfile|grep "] TIMEOUT #"|awk '{print $4,$5}'|sed 's/#//g' >> $logdir/timeouts
        cat $client_logfile|grep "RESPONSE_TIME"|awk '{print $5}' >> $logdir/runtimes
        cat $client_logfile|grep "RESPONSE_TIME"|awk '{print $4,"'$logdir'",$5}'|sed 's/#//g' >> $logdir/qualified_runtimes
        introduced_jobs=$(($introduced_jobs+$introduced))
        
        # scheduling times per job
        while read -r line; do
            if echo $line|grep -q "Introducing job"; then
                starttime=$(echo $line|awk '{print $1}')
                jobid=$(echo $line|grep -oE "#[0-9]+")
                schedtime=$(cat $client_logfile|awk "/Sending job desc. of $jobid/ "'{print $1; exit}')
                echo "$schedtime - $starttime"|bc >> $logdir/scheduling_times
            fi
        done < $client_logfile
        
    done
    echo $introduced_jobs > $logdir/num_introduced_jobs

    sort -n $logdir/responses -o $logdir/responses
    sort -n $logdir/timeouts -o $logdir/timeouts
    sort -n $logdir/runtimes -o $logdir/runtimes
    sort -n $logdir/qualified_runtimes -o $logdir/qualified_runtimes
}

function extract_id_file_map() {
    
    >tmp
    for n in $(get_client_nodes); do
        client_logfile=$logdir/log_*.$n
        grep "FILE_IO reading" $client_logfile|grep -oE "\".*\" \(#[0-9]+\)"|sed 's.[\"#)(]..g' >>tmp
    done
    
    > $logdir/id_file_map
    while read -r line; do
        filename=$(echo $line|awk '{print $1}')
        jobid=$(echo $line|awk '{print $2}')
        echo "$jobid $(basename $filename)" >> $logdir/id_file_map 
    done < tmp
    rm tmp
}

function extract_found_results() {
    
    > $logdir/jobids_SAT
    > $logdir/jobids_UNSAT
    for n in $(get_client_nodes); do
        client_logfile=$logdir/log_*.$n
        grep -oE "SOLUTION #[0-9]+ SAT" $client_logfile|sed 's/#//g'|awk '{print "'$logdir'",$2}' >> $logdir/jobids_SAT
        grep -oE "SOLUTION #[0-9]+ UNSAT" $client_logfile|sed 's/#//g'|awk '{print "'$logdir'",$2}' >> $logdir/jobids_UNSAT
    done
    
    > $logdir/jobs_SAT
    > $logdir/jobs_UNSAT
    while read -r line; do
        jobid=$(echo $line|awk '{print $2}'|sed 's/#//g')
        filename=$(cat $logdir/id_file_map|awk '{if ($1 == "'$jobid'") {print $2}}')
        echo "$logdir $filename" >> $logdir/jobs_SAT
    done < $logdir/jobids_SAT
    
    while read -r line; do
        jobid=$(echo $line|awk '{print $2}'|sed 's/#//g')
        filename=$(cat $logdir/id_file_map|awk '{if ($1 == "'$jobid'") {print $2}}')
        echo "$logdir $filename" >> $logdir/jobs_UNSAT
    done < $logdir/jobids_UNSAT
}

# Takes 2 args: <path of global jobs_SAT> <path of global jobs_UNSAT>
function check_found_results() {
    global_sat="$1"
    global_unsat="$2"
    
    while read -r line ; do
        jobid=$(echo $line|awk '{print $2}')
        if grep -qE " ${jobid}$" "$global_unsat"; then 
            echo "ERROR!"
            grep -E " ${jobid}$" "$global_unsat"
            exit 1
        else
            echo "$logdir $jobid" >> $global_sat
        fi
    done < $logdir/jobs_SAT
    
    while read -r line ; do
        jobid=$(echo $line|awk '{print $2}')
        if grep -qE " ${jobid}$" "$global_sat"; then 
            echo "ERROR!"
            grep -E " ${jobid}$" "$global_sat"
            exit 1
        else
            echo "$logdir $jobid" >> $global_unsat
        fi
    done < $logdir/jobs_UNSAT
}

function get_num_reactivations() {
    cat $logdir/log_*.*|grep "Reactivate"|grep "state: committed"|wc -l
}

function extract_load_events() {
    >ldev
    for f in $logdir/log_*.* ; do
        grep "LOAD" $f >> ldev
    done
    cat ldev|grep -oE "^[0-9]+\.[0-9]+ [0-9]+ LOAD [01] \([+-]#[0-9]+:[0-9]+\)"|awk '{print $1,$2,$4,$5}'|sed 's.(\|)\|\+\|-\|#..g'|sed 's/:/ /g' > $logdir/loadevents
    rm ldev
}

function extract_hop_events() {
    >hpev
    for f in $logdir/log_*.* ; do
        grep "Adopting" $f >> hpev
    done
    sort -g hpev -o hpev
    cat hpev|grep -oE "^[0-9]+\.[0-9]+ [0-9]+ Adopting #[0-9]+:[0-9]+ after [0-9]+ hops"|awk '{print $1,$2,$4,$6}'|sed 's.(\|)\|\+\|-\|#..g'|sed 's/:/ /g' > $logdir/hopevents
    rm hpev
}

function document_node_events() {
    python3 calc_node_events.py $logdir/loadevents $logdir/hopevents >ndev
    grep "NODES " ndev|awk '{$1=""; print $0}' > $logdir/nodes_per_job
    grep "CPUTIME " ndev|awk '{$1=""; print $0}' > $logdir/cpu_times
    grep "MAXNODES " ndev|awk '{$1=""; print $0}' > $logdir/max_nodes_per_job
    grep "CTXSWITCHES " ndev|awk '{$1=""; print $0}' > $logdir/ctxswitches
    grep "HOPS " ndev|awk '{$1=""; print $0}' > $logdir/hopevents_successful
    grep "HOPS " ndev|awk '{print $5}' > $logdir/num_hops
    sort -g $logdir/num_hops -o $logdir/num_hops
    cat $logdir/hopevents_successful|awk '{print $1,$4}' > $logdir/times_hops
    cat $logdir/hopevents_successful|awk '{print $3,$4}' > $logdir/jobidxs_hops
}

function document_hops() {

    nhops=0
    nocc=0 #-$(get_num_reactivations)
    totalocc=0
    > $logdir/num_hops_occurrences
    while read -r x; do
        if [ x"$nhops" == x"$x" ]; then
            nocc=$((nocc+1))
        else
            while [ x"$nhops" != x"$x" ]; do 
                echo $nocc >> $logdir/num_hops_occurrences
                totalocc=$((totalocc+nocc))
                nocc=0
                nhops=$((nhops+1))
            done 
            nocc=1
        fi
    done < $logdir/num_hops
    echo $nocc >> $logdir/num_hops_occurrences
    totalocc=$((totalocc+nocc))
    
    > $logdir/num_hops_density
    while read -r nocc; do
        echo $nocc / $totalocc|bc -l >> $logdir/num_hops_density
    done < $logdir/num_hops_occurrences
    
    > $logdir/num_hops_cdf
    cdf=0
    while read -r x; do
        cdf=$(echo $cdf + $x|bc -l)
        echo $cdf >> $logdir/num_hops_cdf
    done < $logdir/num_hops_density
}

function extract_succeeding_diversifiers() {
    cat $logdir/log_*.*|python3 calc_succeeding_diversifiers.py > $logdir/succeeding_diversifiers
}

function extract_runtime_cputime_mapping() {
    > $logdir/runtime_cputime_map
    while read -r line; do 
        id=$(echo $line|awk '{print $1}')
        if grep -qE "^$id " $logdir/qualified_runtimes; then 
            echo $(cat $logdir/qualified_runtimes|grep -E "^$id "|awk '{print $3}') $(echo $line|awk '{print $3}') >> $logdir/runtime_cputime_map
        fi
    done < $logdir/cpu_times
}

logdir="$1"
if [ "x$1" == "x" ]; then
    echo "No log dir provided"
    exit 1
fi

#flatten_logs

extract_client_info

extract_load_events
extract_hop_events

# Depends: extract_load_events extract_hop_events
document_node_events

# Depends: document_node_events
document_hops

#extract_runtime_cputime_mapping

# CHECK SOLUTIONS
#extract_id_file_map
#extract_found_results
#check_found_results logs/jobs_SAT logs/jobs_UNSAT

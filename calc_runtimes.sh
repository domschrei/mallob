#!/bin/bash

logdir="$1"
if [ "x$1" == "x" ]; then
    echo "No log dir provided"
    exit 1
fi

num_nodes=`ls $logdir/*.*|wc -l`
num_clients=`head $logdir/log_*.0|grep -m1 "Called with parameters"|grep -oE ", c=[0-9]+,"|grep -oE "[0-9]+"`
echo $num_clients clients

startnode=$(($num_nodes-$num_clients))
endnode=$(($num_nodes-1))

> $logdir/responses
> $logdir/timeouts
> $logdir/runtimes
> $logdir/qualified_runtimes

introduced_jobs=0
for n in `seq $startnode $endnode`; do

    client_logfile=$logdir/log_*.$n
    echo $client_logfile
    
    introduced=`grep "Introducing job #" $client_logfile|wc -l`
    cat $client_logfile|grep "RESPONSE_TIME" >> $logdir/responses
    cat $client_logfile|grep "] TIMEOUT #"|awk '{print $4,$5}'|sed 's/#//g' >> $logdir/timeouts
    cat $client_logfile|grep "RESPONSE_TIME"|awk '{print $5}' >> $logdir/runtimes
    cat $client_logfile|grep "RESPONSE_TIME"|awk '{print $4,"'$logdir'",$5}'|sed 's/#//g' >> $logdir/qualified_runtimes
    introduced_jobs=$(($introduced_jobs+$introduced))
done
echo $introduced_jobs > $logdir/num_introduced_jobs

sort -n $logdir/responses -o $logdir/responses
sort -n $logdir/timeouts -o $logdir/timeouts
sort -n $logdir/runtimes -o $logdir/runtimes
sort -n $logdir/qualified_runtimes -o $logdir/qualified_runtimes

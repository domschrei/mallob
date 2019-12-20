#!/bin/bash

logdir="$1"
if [ "x$1" == "x" ]; then
    echo "No log dir provided"
    exit 1
fi

num_nodes=`ls $logdir/*.*|wc -l`
client_logfile=$logdir/log_*.$(($num_nodes-1))

cat $client_logfile|grep "RESPONSE_TIME"|awk '{print $7}'|sort -n > $logdir/runtimes
cat $client_logfile|grep "RESPONSE_TIME"|awk '{print $6,"'$logdir'",$7}'|sed 's/#//g'|sort -n > $logdir/qualified_runtimes

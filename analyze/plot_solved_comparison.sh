#!/bin/bash

shift 1
cmd="python3 analyze/plot_solved_instances.py"
for d in $@ ; do
    echo $d
    args=`cat $d/args|sed 's/ /_/g'`
    echo $args
    #cmd=$cmd" $d/runtimes -l\"$args\""
done

#echo $cmd

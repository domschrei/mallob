#!/bin/bash

#set -e

function gettrueresult() {
    trueresult=$(cat instances/metadata|grep "$1"|awk '{print $3}')
    if [ $trueresult != sat ] && [ $trueresult != unsat ] && [ $trueresult != unknown ]; then
        echo "Internal error: Instance $instance not found"
        #exit 1
    fi
    echo $trueresult
}

# Uses ext. variable "trueresult"
function checksol() {
    result=$1
    if [ $result == SAT ]; then
        if [ $trueresult == unsat ]; then
            echo "ERROR: $instance result: $result true result: $trueresult"
            #exit 1
        fi
    fi
    if [ $result == UNSAT ]; then
        if [ $trueresult == sat ]; then
            echo "ERROR: $instance result: $result true result: $trueresult"
            #exit 1
        fi
    fi
    #echo "OK: $result $trueresult"
}

infile="$1"

if grep -q "mono instance" "$infile" || grep -q "Initiate solving" "$infile"; then
    
    # mono Mode
    instance=$(cat "$infile"|grep -m 1 -oE "\".*\.cnf\""|sed 's/"//g'|sed 's,.*/,,g')
    echo "$infile" $instance
    
    # Get true result
    trueresult=$(gettrueresult "$instance")
        
    # Check reported result(s)
    cat "$infile"|grep -oE -m 1 "result (UN)?SAT"|while read line; do
        checksol $(echo $line|grep -oE "(UN)?SAT")
    done

else
    
    # Scheduling mode
    echo "Scheduling!"
    
    cat "$infile"|grep -oE "SOLUTION #[0-9]+ (UN)?SAT"|while read line; do
        
        identifier=$(echo $line|awk '{print $2}')
        instance=$(cat $infile|grep -m 1 "Reading job $identifier "|grep -oE "\(.*\)"|sed 's/(//g'|sed 's/)//g'|sed 's,.*/,,g')
        
        echo $infile $identifier $instance
        
        result=$(echo $line|awk '{print $3}')
        trueresult=$(gettrueresult "$instance")
        checksol $result
    done

fi

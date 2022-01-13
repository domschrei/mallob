#!/bin/bash

function gettrueresult() {
    trueresult=$(cat instances/metadata|grep "$1"|awk '{print $3}')
    if [ "$trueresult" != sat ] && [ "$trueresult" != unsat ] && [ "$trueresult" != unknown ]; then
        echo " Internal error: Instance $1 not found - skipping"
        exit 1
    fi
    export trueresult=$trueresult
}

# Uses ext. variable "trueresult"
function checksol() {
    result=$1
    if [ $result == 10 ]; then
        if [ $trueresult == unsat ]; then
            echo ""
            echo "ERROR: $instance ; result: $result ; true result: $trueresult"
            echo ""
            exit 1
        fi
    fi
    if [ $result == 20 ]; then
        if [ $trueresult == sat ]; then
            echo ""
            echo "ERROR: $instance ; result: $result ; true result: $trueresult"
            echo ""
            exit 1
        fi
    fi
    #echo "OK: $result $trueresult"
}

if [ -z "$1" ]; then
    echo "Provide a JSON file path."
    exit 1
fi
jsonfile="$1"

formulafile=$(jq -r ".files[0]" "$jsonfile"|sed 's,^instances/,,g')

echo Checking $formulafile ...

gettrueresult "$formulafile"

echo " True result: $trueresult"

code=$(jq ".result.resultcode" "$jsonfile")

echo " Found code: $code"

checksol $code

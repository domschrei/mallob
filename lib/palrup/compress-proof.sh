#!/bin/bash

mode="$1" # XZ or VASKIN_GOETZ
input="$2" # path to pipe (already set up)
output="$3" # path to plain file

if [ "x$mode" == "xXZ" ]; then
    xz -k -z -c -T 1 "$input" > "$output"
elif [ "x$mode" == "xVASKIN_GOETZ" ]; then
    # TODO call Rust executable to compress "$input" into "$output"
    echo "--ERROR-- proof compression mode VASKIN_GOETZ not yet implemented - exiting"
    exit 1
else
    echo "--WARNING-- Unknown proof compression mode \"$mode\" - defaulting to no compression" 
    cat "$input" > "$output"
fi

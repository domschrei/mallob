#!/bin/bash

if [ -z "$1" ]; then
    echo "Usage: $0 arg1 [arg2 ...]"
    echo "For a sequence of Mallob arguments of the form "-key=val" (single dash and "="!),"
    echo "filters out the default option arguments and prints the non-default arguments only."
    exit 0
fi

build/mallob -h | grep " default: " | awk '{print $1"="$NF; if (NF>4) {print $3"="$NF}}' | sed 's/)//g' > .default-options

out=""
while [ ! -z "$1" ]; do
    opt="$1"
    if ! grep -qE "^$opt$" .default-options ; then
        out="$out$opt "
    fi
    shift 1
done

echo $out

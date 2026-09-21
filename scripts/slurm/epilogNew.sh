#!/bin/bash

build=$MALLOB_BUILDDIR
globallogdir=$MALLOB_GLOBALLOGDIR
localtmpdir=$MALLOB_LOCALTMPDIR
outputlogdir=$MALLOB_OUTPUTLOGDIR
numnodes=$MALLOB_NUMNODES

if [ -z "$globallogdir" ]; then exit; fi
if [ -z "$localtmpdir" ]; then exit; fi

# Node-local lock, released on any exit
if ! mkdir /tmp/.epilog.lock 2>/dev/null ; then exit ; fi
trap 'rmdir /tmp/.epilog.lock 2>/dev/null' EXIT

dest="$outputlogdir/$(basename "$globallogdir")"
mkdir -p "$dest"
if [ -f "$dest/.alldone" ]; then exit ; fi

>&2 echo "$(date) EPILOG $(hostname): $build $globallogdir $outputlogdir"
>&2 echo "$(date) EPILOG $(hostname): DEST: $dest"


# Cross-node lock on the SHARED filesystem: exactly one node does the move
if mkdir "$dest/.movelock" 2>/dev/null ; then
    prevdir=$(pwd)
    cd "$globallogdir" || exit 1
    for x in * ; do
        [ -e "$x" ] || continue
        if [ -d "$x" ]; then
            mkdir -p "$dest/$x"
            mv -- "$x"/* "$dest/$x/" &
        elif [ -f "$x" ]; then
            mv -- "$x" "$dest/" &
        fi
    done
    wait
    cd "$prevdir"
fi

touch "$dest/.done.$(hostname)"
while [ "$(ls -1 "$dest"/.done.* 2>/dev/null | wc -l)" -lt "${numnodes:-1}" ]; do sleep 1; done
touch "$dest/.alldone"

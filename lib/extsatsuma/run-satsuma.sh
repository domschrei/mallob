#!/bin/bash

# Forwards termination to Satsuma subprocess
function cleanup() {
    trap - SIGTERM SIGINT EXIT
    local mypid=$$
    if [ "$pid_satsuma" -gt 0 ]; then
        kill -9 "$pid_satsuma" 2>/dev/null
    fi
    exit 143
}
trap cleanup SIGTERM SIGINT EXIT

MALLOB_SUBPROC_DISPATCH_PATH="$1"
INPUT="$2"
OUTPUT="$3"
LOG="$4"
pid_satsuma=0

cat "$INPUT" | \
 "$MALLOB_SUBPROC_DISPATCH_PATH/satsuma" fix --add-reduced-as-unit --out-file "$OUTPUT" \
 > "$LOG" 2>&1 &

pid_satsuma=$!
while ps -p $pid_satsuma > /dev/null; do
    sleep 0.2
done

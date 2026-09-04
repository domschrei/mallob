#!/bin/bash

MALLOB_SUBPROC_DISPATCH_PATH="$1"
INPUT="$2"
OUTPUT="$3"
LOG="$4"

cat "$INPUT" | \
 "$MALLOB_SUBPROC_DISPATCH_PATH/satsuma" fix --add-reduced-as-unit --out-file "$OUTPUT" \
 > "$LOG" 2>&1

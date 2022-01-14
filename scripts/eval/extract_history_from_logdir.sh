#!/bin/bash

logdir="$1"
if [ -z "$logdir" ] || [ ! -d "$logdir" ]; then
    echo "Provide a valid log directory."
    exit 1
fi

outfile="loadevents"
if [ ! -z "$2" ]; then
    outfile="$2"
fi

# Merge log files
bash scripts/eval/merge_rank_log_files.sh $logdir

# Extract "LOAD" and "warmup msg" events from the directory
cat $logdir/*/*.log|grep -E "LOAD|warmup msg" > runlog

# Harmonize timestamps using warmup messages, write into runlog.harmonized
python3 scripts/eval/harmonize_timestamps.py runlog

# Format and stable sort harmonized load events
export LC_ALL=C
cat runlog.harmonized |grep LOAD|sed 's/[#)(+:-]/ /g'|sed 's/  \+/ /g'|sort -g -s|awk '{print $1,$2,$4,$5,$6}' > "$outfile"

echo Wrote to "$outfile".

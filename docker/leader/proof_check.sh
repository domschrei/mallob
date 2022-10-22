#!/bin/bash

# Decompress the compressed LRAT file so that it can be checked by lrat-check
time1=$(date +"%s.%3N")
/decompress /logs/processes/renumbered.lrat > /logs/processes/renumbered-dec.lrat
time2=$(date +"%s.%3N")
echo "Decompressed proof ($(echo $time1 $time2|awk '{print $2-$1}')s)" # TODO add proper timing string

# Check the decompressed proof
/lrat-check $1 /logs/processes/renumbered-dec.lrat

#!/bin/bash

echo "Configuration & \# & PAR2 \\"
for d in RESULTS/*/ ; do 
    if [ ! -f $d/par2score ]; then continue; fi
    name=$(echo $d\
    |sed 's,RESULTS,,g'\
    |sed 's,mallob_mono_,Mallob_$,g'\
    |sed 's,horde_new_,Hordesat_(new)_$,g'\
    |sed 's,horde_old_,Hordesat_(old)_$,g'\
    |sed 's,/,,g'\
    |sed 's/cbdf/\\alpha=/g'\
    |sed 's/mlbd/LBD=/g'\
    |sed 's/mcl0//g'\
    |sed 's/mcl/MCL=/g'\
    |sed 's/_n/_m=/g'\
    |sed 's/\$n/$m=/g'\
    |sed 's/$/$/g'\
    |sed 's/cfhl/X=/g')
    echo $(cat $d/par2score) $(cat $d/cdf_runtimes|wc -l) "$name"
done |sort -g|awk '{print $3" & "$2 " & "$1" \\\\"}'|column -t|sed 's/_/ /g'

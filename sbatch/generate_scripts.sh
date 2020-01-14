#!/bin/bash

c=1
while read -r line; do
    if echo $line|grep -q '#!/bin/bash'; then
        echo $line
        echo $line|grep -oE '#!/bin/bash.*echo finished'|sed 's/<br>/\n/g'|sed 's/<\/?p>//g' > script_$c.sh
        id=`cat script_$c.sh|grep job-name|grep -oE "mallob_id[0-9]+"|grep -oE "[0-9]+"`
        mv script_$c.sh script_id$id.sh
        c=$(($c+1))
    fi
done < $1

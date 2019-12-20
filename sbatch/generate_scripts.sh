#!/bin/bash

c=1
while read -r line; do
    if echo $line|grep -q '#!/bin/bash'; then
        echo $line|grep -oE '#!/bin/bash.*echo finished'|sed 's/<br>/\n/g' > script_$c.sh
        c=$(($c+1))
    fi
done < $1

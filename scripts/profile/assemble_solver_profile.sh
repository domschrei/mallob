#!/bin/bash

if [ -z "$1" ]; then
    echo "Usage: assemble_solver_profile.sh <base-directory>"
fi

basedir="$1"

for d in $basedir/*/ ; do
    if [ -f $d/compact-profile.txt ]; then 
        continue
    fi
    i=$(basename $d)
    echo $i $(cat $d/instance.txt); cat $d/profile.* | sed 's/%//g' \
    | awk 'NF==3 && $2>0 {sum[$3]+=log($2); n[$3]+=1} END {for (k in sum) {print k, n[k], exp(sum[k] / n[k])}}' \
    | sort -k 1,1b > $d/compact-profile.txt
done

cat $basedir/*/compact-profile.txt | awk '$3 <= 100 {print $3,$1}' | sort -g \
| awk '{a[$2] = a[$2]" "$1} END {for (k in a) {print(k""a[k])}}' \
| sort -k 1,1b > $basedir/profile.txt

# Assemble by-instance profiling table.
# A particular category can be "queried" via, e.g.:
# cat profile-by-instance.txt | sed 's/ .*ternary//g' | awk '{print $2,$1}' | sort -g
for f in $basedir/*/compact-profile.txt ; do
    d=$(dirname $f)
    echo $(basename $(cat $d/instance.txt)) $(cat $f|awk '{printf($1" "$3" ")}')
done | sort -k 1,1b > $basedir/profile-by-instance.txt 

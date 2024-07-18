#!/bin/bash

if [ -z "$1" ]; then
    echo "Usage: assemble_solver_profile.sh <outputfile> (Mallob-mono output directories w/ profiles inside)"
fi

rm .profile.* 2>/dev/null || :

mkdir -p .profile-tmpfiles

output="$1"
shift 1

for d in $@ ; do

    echo $d

    for f in $d/profile.* ; do
        slv=$(echo $f|grep -oE "[0-9]+$")
        if [ -f "$(dirname $f)/instance.txt" ]; then
            inst=$(cat $(dirname $f)/instance.txt|sed 's,.*/,,g')
        else
            inst="?"
        fi
        sed 's/%//g' $f | awk 'NF==3 {print "'$inst'","'$slv'",$3,$1,0.01*$2}' > .profile-tmpfiles/tmpprofile.$(basename $f) &
    done
    
    wait
    cat .profile-tmpfiles/tmpprofile.* > .profile-tmpfiles/profile.$(basename $d)
    rm .profile-tmpfiles/tmpprofile.*
done

wait

echo "Final merge"
cat .profile-tmpfiles/profile.* > $output
rm .profile-tmpfiles/profile.*

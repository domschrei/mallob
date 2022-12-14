#!/bin/bash

if [ -z $APP ]; then
	APP="SAT"
fi
if [ -z $WCLIMIT ]; then
	WCLIMIT="0"
fi
if [ -z $APIDIR ]; then
	APIDIR=".api/jobs.0/"
fi

user="admin"
jobname="autojob-$(date +%s-%N)"
echo '{"user": "'$user'", "name": "'$jobname'", "application": "'$APP'", "files": ["'$1'"], "wallclock-limit": "'$WCLIMIT'"}' > .user.job.json
mv .user.job.json "${APIDIR}/in/$user.$jobname.json"


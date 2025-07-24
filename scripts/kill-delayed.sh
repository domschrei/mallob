#!/bin/sh
exec </dev/null
exec >> /dev/null
exec 2>> /dev/null
setsid nohup bash scripts/do-kill-delayed.sh $1 & disown

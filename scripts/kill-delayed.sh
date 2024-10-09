#!/bin/sh
exec </dev/null
exec >> ~/out.log
exec 2>> ~/err.log
setsid nohup bash -c "for i in {1..10}; do if ps -p $1; then sleep 0.5; fi; done; kill -9 $1" & disown

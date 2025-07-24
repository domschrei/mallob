#!/bin/bash

for i in {1..10}; do
    if ! ps -p $1; then exit; fi
    sleep 0.5
done
if ! ps -p $1; then exit; fi
kill -18 $1
kill -15 $1
sleep 5
if ! ps -p $1; then exit; fi
kill -9 $1

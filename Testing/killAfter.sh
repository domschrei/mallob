#!/bin/bash

sleep ${1}
killall -9 mpirun ; killall -9 build/mallob

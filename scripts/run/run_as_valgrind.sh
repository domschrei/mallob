#!/bin/bash

# Runs the command with basic valgrind (i.e., with the memcheck tool).
opts="--track-origins=yes --keep-stacktraces=alloc-and-free"
PATH=build:.:$PATH RDMAV_FORK_SAFE=1 exec -a=/usr/bin/valgrind /usr/bin/valgrind $opts $@

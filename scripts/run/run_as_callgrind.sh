#!/bin/bash

# Runs the command with valgrind with the callgrind tool, generating profiling information on the run time performance.
# The resulting files can be visualized with the tool KCacheGrind.

PATH=build:.:$PATH RDMAV_FORK_SAFE=1 exec -a=/usr/bin/valgrind /usr/bin/valgrind --tool=callgrind --separate-threads=yes --dump-instr=yes $@

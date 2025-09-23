#!/bin/bash

# Runs the command with valgrind with the callgrind tool, generating profiling information on the run time performance.
# The resulting files can be visualized with the tool KCacheGrind.
echo "run_as_valgrins.sh  executed"

PATH=build:.:$PATH RDMAV_FORK_SAFE=1 exec -a=/usr/bin/valgrind /usr/bin/valgrind --tool=callgrind --separate-threads=yes --dump-instr=yes $@
